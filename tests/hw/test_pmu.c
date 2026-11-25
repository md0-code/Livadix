/* test_pmu: the PowerBook Power Manager (pmu.c, pmu_fw.c) through the VIA
 * side, as the ROMs' Power Manager driver (PMgrOp) drives it: the parallel
 * handshake (port A, /REQ, /ACK), the shift-register one (VIA1 SR clocked by
 * the PMU), the old and new framings; every command class, the interrupts
 * and their acknowledge, ADB with autopoll, sleep and the wake hook, PRAM
 * persistence through the emulation.nvram file.
 *
 *   mc_test_pmu                 the protocol level (and the differential
 *                               when the firmware is in MC_ROMS)
 *   mc_test_pmu --diff ROMS     the differential only: protocol and
 *                               firmware levels on the same command
 *                               sequences; exit 77 when the firmware is
 *                               not in ROMS
 *   mc_test_pmu --trace ROMS    prints the VIA-side events of both levels
 *
 * Expected values come from the protocol pmu.h describes (the firmware and
 * the ROMs' PMgrOp as observed); the receive counts the
 * driver uses for the new framing are typed here from the ROM's table. */
#include "hw_test.h"
#include "adb.h"
#include "pmu.h"
#include "pmu_fw.h"

#include "config.h"
#include "romdb.h"

#include <stdlib.h>
#include <string.h>

#ifndef MC_ROMS
#define MC_ROMS "roms"
#endif

static int tests;
static int trace;

/* --- the machine around the PMU ---------------------------------------------------- */

#define EVENTS 65536
typedef struct event {
    uint64_t t;
    int line, state;
} event;

/* The Portable's 60.15 Hz (machines/portable.c): a square wave of 704 x
 * 370 C15M periods on the PMU's tick input. */
typedef struct rig_tick {
    mc_dev dev;
    mc_pmu *pmu;
    uint8_t level;
    uint64_t next;
} rig_tick;

static void rig_tick_event(void *dev, uint64_t now) {
    rig_tick *t = dev;
    while (t->next <= now) {
        t->level ^= 1u;
        mc_pmu_set_tick(t->pmu, t->level);
        t->next += 704u * 370u / 2u;
    }
    mc_dev_schedule(&t->dev, t->next);
}
static const mc_dev_ops rig_tick_ops = {NULL, NULL, rig_tick_event, NULL, NULL};

typedef struct rig {
    hw_fake fake;
    rig_tick tick;
    mc_pmu pmu;
    mc_adb_bus bus;
    mc_adb_keyboard kbd;
    mc_adb_mouse mouse;
    event ev[EVENTS];
    unsigned nev;
    unsigned int_pulses;
    int error;
    /* The shift-register interface: the VIA's SR. */
    uint8_t sr, sr_out_mode;
} rig;

static rig *current;

static void on_line(hw_fake *f, void *dev, int line, int state) {
    rig *r = current;
    (void)dev;
    if (r->nev < EVENTS) {
        r->ev[r->nev].t = f->now;
        r->ev[r->nev].line = line;
        r->ev[r->nev].state = state;
        r->nev++;
    }
    if (line == MC_PMU_LINE_INT && state == 0) r->int_pulses++;
}

/* VIA1's shift register under the PMU's clock (mc_via_shift8's contract):
 * shift-out mode gives the Mac's byte, shift-in takes the PMU's. */
static uint8_t rig_shift(void *ctx, uint8_t in) {
    rig *r = ctx;
    const uint8_t out = r->sr;
    if (!r->sr_out_mode) r->sr = in;
    return out;
}

static void rig_init(rig *r, unsigned model, unsigned iface, const uint8_t *fw, size_t fw_size) {
    memset(r, 0, sizeof *r);
    current = r;
    hw_fake_init(&r->fake);
    r->fake.on_line = on_line;
    mc_adb_bus_init(&r->bus);
    mc_adb_keyboard_init(&r->kbd);
    mc_adb_mouse_init(&r->mouse);
    mc_adb_attach(&r->bus, &r->kbd.dev);
    mc_adb_attach(&r->bus, &r->mouse.dev);
    mc_pmu_init(&r->pmu, &r->fake.host, "PMU", model, iface, &r->bus, fw, fw_size);
    mc_pmu_set_shift(&r->pmu, rig_shift, r);
    r->pmu.dev.ops->reset(&r->pmu);
    if (model == MC_PMU_PORTABLE) {
        r->tick.dev.ops = &rig_tick_ops;
        r->tick.dev.host = &r->fake.host;
        r->tick.dev.name = "60.15 Hz";
        r->tick.pmu = &r->pmu;
        r->tick.level = 1;
        r->tick.next = 704u * 370u / 2u;
        mc_dev_schedule(&r->tick.dev, r->tick.next);
    }
}

static void rig_free(rig *r) { mc_pmu_free(&r->pmu); }

#define STEP (MC_C15M_HZ / 1000000u * 2u)        /* the driver polls every 2 us */
#define US(n) ((uint64_t)MC_C15M_HZ * (n) / 1000000u)
#define MS(n) ((uint64_t)MC_C15M_HZ * (n) / 1000u)

static void run_for(rig *r, uint64_t periods) { hw_fake_step(&r->fake, periods); }

static uint64_t reply_timeout = (uint64_t)MC_C15M_HZ * 32u / 1000u;

static int wait_ack(rig *r, int level, uint64_t timeout) {
    if (timeout < reply_timeout) timeout = reply_timeout;
    const uint64_t end = r->fake.now + timeout;
    while (r->pmu.ack != level) {
        if (r->fake.now >= end) return 0;
        hw_fake_step(&r->fake, STEP);
    }
    return 1;
}

static void req(rig *r, int level) {
    if (r->nev < EVENTS) {
        r->ev[r->nev].t = r->fake.now;
        r->ev[r->nev].line = -1;
        r->ev[r->nev].state = level;
        r->nev++;
    }
    mc_pmu_set_req(&r->pmu, level);
    hw_fake_step(&r->fake, STEP);
}

/* The ROM's byte transfers (PB 160 ROM $89AD4 send, $89A76 receive). */
static double byte_lat[64];
static unsigned nbyte_lat;
static void note_lat(rig *r, uint64_t t0) {
    if (nbyte_lat < 64) byte_lat[nbyte_lat++] = (double)(r->fake.now - t0) * 1e6 / MC_C15M_HZ;
}

static void send_byte(rig *r, uint8_t b) {
    if (r->error) return;
    const uint64_t t0 = r->fake.now;
    if (r->pmu.iface == MC_PMU_SHIFT) {
        r->sr = b;
        r->sr_out_mode = 1;
    } else {
        mc_pmu_set_port(&r->pmu, b);
    }
    req(r, 0);
    if (!wait_ack(r, 0, MS(32))) { r->error = 1; req(r, 1); return; }
    note_lat(r, t0);
    req(r, 1);
    if (!wait_ack(r, 1, MS(32))) r->error = 2;
}

static uint8_t recv_byte(rig *r) {
    if (r->error) return 0;
    uint8_t b;
    if (r->pmu.iface == MC_PMU_SHIFT) {
        r->sr_out_mode = 0;
        req(r, 0);
        if (!wait_ack(r, 0, MS(32))) { r->error = 3; req(r, 1); return 0; }
        req(r, 1);
        if (!wait_ack(r, 1, MS(32))) r->error = 4;
        return r->sr;
    }
    mc_pmu_set_port(&r->pmu, 0xFF);           /* port A an input */
    const uint64_t t0 = r->fake.now;
    if (!wait_ack(r, 0, MS(32))) { r->error = 3; return 0; }
    note_lat(r, t0);
    req(r, 0);
    b = r->pmu.data_out < 0 ? 0xFF : (uint8_t)r->pmu.data_out;
    if (!wait_ack(r, 1, MS(32))) r->error = 4;
    req(r, 1);
    return b;
}

/* The receive counts of the new framing (PB 160 ROM $89EAC) for the
 * commands used here. */
static unsigned rom_recv(uint8_t c) {
    switch (c) {
    case 0x18: case 0x19: case 0x48: case 0x49: case 0x58: case 0x98: case 0xD8: case 0xEA: return 2;
    case 0x3B: return 2;
    case 0x38: return 5;
    case 0x39: return 21;
    case 0x68: case 0x69: return 4;
    case 0x6A: return 3;
    case 0x7E: case 0x7F: case 0xE2: return 1;
    case 0x88: return 6;
    default: return (c & 0x08u) ? 0xFFu : 0u;
    }
}
static unsigned rom_send(uint8_t c) {
    switch (c) {
    case 0x10: case 0x40: case 0x41: case 0x50: case 0x90: case 0xD8: return 1;
    case 0x21: case 0x18: case 0x38: case 0x39: case 0x48: case 0x49: case 0x58: case 0x68: case 0x69:
    case 0x6A: case 0x78: case 0x82: case 0x88: case 0x98: case 0xE2: case 0xEA: case 0xEC: case 0xEF: return 0;
    case 0x30: case 0x7E: case 0x7F: case 0x80: case 0xE1: return 4;
    case 0x31: return 20;
    case 0x33: return 3;
    case 0x3B: case 0x3A: case 0x60: return 2;
    case 0xE8: return 3;
    default: return 0xFF;
    }
}

typedef struct op_result {
    int error;
    uint8_t first;
    uint8_t data[256];
    unsigned n;
} op_result;

/* PMgrOp (PB 160 ROM $8987A-$89A38): the PB 160's busy wait, the command,
 * the count where the framing wants it, the data, then the reply. */
static op_result pmgr_op(rig *r, uint8_t cmd, const uint8_t *data, unsigned n) {
    op_result res;
    memset(&res, 0, sizeof res);
    nbyte_lat = 0;
    const int newf = r->pmu.model == MC_PMU_PGE || r->pmu.model == MC_PMU_99;
    r->error = 0;
    if (r->pmu.model == MC_PMU_PB160) {
        const uint64_t end = r->fake.now + MS(20);
        while (r->pmu.data_out == 0x07 && r->fake.now < end) hw_fake_step(&r->fake, STEP);
    }
    send_byte(r, cmd);
    if (!newf || rom_send(cmd) == 0xFF) send_byte(r, (uint8_t)n);
    for (unsigned i = 0; i < n; i++) send_byte(r, data[i]);
    const unsigned rc = newf ? rom_recv(cmd) : 0xFFu;
    unsigned want = 0;
    if (!newf) {
        /* Old framing: the caller knows whether a reply comes; here every
         * read command (bit 3) and those listed. */
        if ((cmd & 0x08u) || cmd == 0x7E || cmd == 0x7F || cmd == 0x3A || cmd == 0x38 || cmd == 0xE8) {
            res.first = recv_byte(r);
            want = recv_byte(r);
        }
    } else if (rc == 0xFF) {
        want = recv_byte(r);
    } else if (rc == 1) {
        res.first = recv_byte(r);
    } else if (rc) {
        want = rc - 1;
    }
    for (unsigned i = 0; i < want && i < sizeof res.data; i++) res.data[res.n++] = recv_byte(r);
    res.error = r->error;
    return res;
}
#define OP(r, cmd, ...) pmgr_op(r, cmd, (const uint8_t[]){0, __VA_ARGS__} + 1, sizeof((const uint8_t[]){0, __VA_ARGS__}) - 1)
#define OP0(r, cmd) pmgr_op(r, cmd, NULL, 0)

/* Runs until the PMU powers the machine on and enters its main loop (four
 * ticks later), as the ROM's first PMU command comes much later still. */
static int power_up(rig *r) {
    const uint64_t end = r->fake.now + MS(2000);
    while (!(r->pmu.power == MC_PMU_POWER_ON && !r->pmu.reset) && r->fake.now < end) run_for(r, US(500));
    run_for(r, MS(50));
    return r->pmu.power == MC_PMU_POWER_ON;
}

/* Waits for an interrupt pulse and reads its data with $78, as the ROM's
 * CB1 handler does (PB 160 ROM $885EC). */
static double int_wait;

static op_result take_interrupt(rig *r, uint64_t timeout) {
    const unsigned before = r->int_pulses;
    const uint64_t end = r->fake.now + timeout;
    const uint64_t t0 = r->fake.now;
    while (r->int_pulses == before && r->fake.now < end) run_for(r, US(50));
    int_wait = (double)(r->fake.now - t0) * 1e6 / MC_C15M_HZ;
    op_result res;
    if (r->int_pulses == before) {
        memset(&res, 0, sizeof res);
        res.error = -1;
        return res;
    }
    return OP0(r, 0x78);
}

/* A firmware image found through the ROM index of the folder `roms`, as
 * the machines find it (romdb.h); NULL when absent. */
static uint8_t *load_firmware(const char *roms, const char *key, size_t *size) {
    char folder[1024];
    snprintf(folder, sizeof folder, "%s/", roms);
    const char *folders[] = {folder};
    mc_rom_index ix;
    memset(&ix, 0, sizeof ix);
    uint8_t *data = NULL;
    *size = 0;
    if (mc_rom_index_scan(&ix, folders, 1)) {
        const mc_rom_file *file = mc_rom_index_firmware(&ix, mc_firmware_find(key));
        FILE *f = file ? fopen(file->path, "rb") : NULL;
        if (f) {
            data = malloc((size_t)file->size);
            if (data && fread(data, 1, (size_t)file->size, f) == file->size) *size = (size_t)file->size;
            else { free(data); data = NULL; }
            fclose(f);
        }
        mc_rom_index_free(&ix);
    }
    return data;
}

/* emulation.controller through the index: protocol never runs the
 * firmware, auto runs it when found, firmware requires it. */
static void test_select(const char *roms, unsigned model, const char *key) {
    char folder[1024];
    snprintf(folder, sizeof folder, "%s/", roms);
    const char *folders[] = {folder};
    const char *keys[] = {key, NULL}, *none[] = {NULL};
    mc_rom_index ix;
    memset(&ix, 0, sizeof ix);
    if (!mc_rom_index_scan(&ix, folders, 1)) return;
    if (mc_rom_index_firmware(&ix, mc_firmware_find(key))) {
        tests++;
        rig *r = malloc(sizeof *r);
        rig_init(r, model, MC_PMU_PARALLEL, NULL, 0);
        CHECK_EQ(mc_pmu_select(&r->pmu, MC_CONTROLLER_PROTOCOL, &ix, keys), 0);
        CHECK_EQ(r->pmu.level, MC_PMU_PROTOCOL);
        CHECK_EQ(mc_pmu_select(&r->pmu, MC_CONTROLLER_AUTO, &ix, none), 0);
        CHECK_EQ(mc_pmu_select(&r->pmu, MC_CONTROLLER_FIRMWARE, &ix, none), -1);
        CHECK_EQ(mc_pmu_select(&r->pmu, MC_CONTROLLER_AUTO, &ix, keys), 1);
        CHECK_EQ(r->pmu.level, MC_PMU_FIRMWARE);
        rig_free(r);
        rig_init(r, model, MC_PMU_PARALLEL, NULL, 0);
        CHECK_EQ(mc_pmu_select(&r->pmu, MC_CONTROLLER_FIRMWARE, &ix, keys), 1);
        rig_free(r);
        free(r);
    }
    mc_rom_index_free(&ix);
}

/* --- protocol level ------------------------------------------------------------------ */

static void test_power_up_and_version(unsigned model) {
    tests++;
    rig *r = malloc(sizeof *r);
    rig_init(r, model, MC_PMU_PARALLEL, NULL, 0);
    CHECK_EQ(r->pmu.power, MC_PMU_POWER_OFF);
    CHECK(power_up(r));
    op_result v = OP0(r, 0xEA);
    CHECK_EQ(v.error, 0);
    CHECK_EQ(v.first, 0xEA);
    CHECK_EQ(v.n, 2);
    CHECK_EQ(v.data[0], model == MC_PMU_PB140 ? 0x02 : 0x03);
    CHECK_EQ(v.data[1], model == MC_PMU_PB140 ? 0xB3 : 0xA2);
    reply_timeout = MS(1000);               /* the self-test takes a quarter second */
    op_result t = OP0(r, 0xEC);
    reply_timeout = MS(32);
    CHECK_EQ(t.n, 1);
    CHECK_EQ(t.data[0], 0);
    rig_free(r);
    free(r);
}

static void test_power_planes_contrast_modem(void) {
    tests++;
    rig *r = malloc(sizeof *r);
    rig_init(r, MC_PMU_PB140, MC_PMU_PARALLEL, NULL, 0);
    CHECK(power_up(r));
    op_result s = OP0(r, 0x18);
    CHECK_EQ(s.first, 0x18);
    CHECK_EQ(s.data[0], ((0x40 & 0x7F) ^ 0x77) & 0xDF);
    OP(r, 0x10, 0x84);                      /* bit 7: clear plane bit 2 */
    OP(r, 0x10, 0x01);                      /* set plane bit 0 */
    s = OP0(r, 0x18);
    CHECK_EQ(s.data[0], (((0x40 | 0x01) & 0x7F) ^ 0x77) & 0xDF);
    CHECK_EQ(hw_fake_value(&r->fake, &r->pmu, MC_PMU_LINE_PLANES), 0x41);
    OP(r, 0x40, 0x17);
    CHECK_EQ(hw_fake_value(&r->fake, &r->pmu, MC_PMU_LINE_CONTRAST), 0x17);
    s = OP0(r, 0x48);
    CHECK_EQ(s.data[0], 0x17);
    OP(r, 0x90, 0x01);
    CHECK_EQ(hw_fake_value(&r->fake, &r->pmu, MC_PMU_LINE_MODEM), 1);
    s = OP0(r, 0x98);
    CHECK_EQ(s.data[0], 0x03);
    OP(r, 0x50, 0x05);                      /* misc. plane on, wake on ring */
    s = OP0(r, 0x58);
    CHECK_EQ(s.data[0], 0x05);
    s = OP(r, 0xD8, 0x01);                  /* A-D channel 1: the battery */
    CHECK_EQ(s.data[0], MC_PMU_BATTERY_LEVEL);
    rig_free(r);
    free(r);
}

static void test_pram_and_clock(unsigned model) {
    tests++;
    rig *r = malloc(sizeof *r);
    rig_init(r, model, MC_PMU_PARALLEL, NULL, 0);
    CHECK(power_up(r));
    uint8_t orig[20];
    for (int i = 0; i < 20; i++) orig[i] = (uint8_t)(0xA0 + i);
    op_result w = pmgr_op(r, 0x31, orig, 20);
    CHECK_EQ(w.error, 0);
    op_result rd = OP0(r, 0x39);
    CHECK_EQ(rd.n, 20);
    /* PRAM byte 8 is the PB 160 firmware's power-key counter. */
    if (model == MC_PMU_PB160) orig[16] = 0;
    CHECK(!memcmp(rd.data, orig, 20));
    OP(r, 0x32, 0x40, 0x03, 0x11, 0x22, 0x33);
    rd = OP(r, 0x3A, 0x3F, 0x05);
    CHECK_EQ(rd.first, 0x3A);
    CHECK_EQ(rd.n, 5);
    CHECK_EQ(rd.data[0], 0);
    CHECK_EQ(rd.data[1], 0x11);
    CHECK_EQ(rd.data[3], 0x33);
    rd = OP(r, 0x3A, 0x10, 0x02);             /* $31 wrote PRAM $10.. */
    CHECK_EQ(rd.data[0], 0xA0);
    rd = OP(r, 0x3A, 0x09, 0x01);             /* and $08.. */
    CHECK_EQ(rd.data[0], 0xB1);
    rd = OP(r, 0xE8, 0x00, 0x30 + 0x41, 0x01);   /* PMU memory $71 is PRAM $41 */
    CHECK_EQ(rd.data[0], 0x22);
    if (model != MC_PMU_PB140) {
        OP(r, 0x30, 0xB0, 0x00, 0x00, 0x10);
        rd = OP0(r, 0x38);
        CHECK_EQ(rd.n, 4);
        CHECK_EQ(rd.data[0], 0xB0);
        CHECK_EQ(rd.data[3], 0x10);
        run_for(r, MS(2100));
        rd = OP0(r, 0x38);
        CHECK(rd.data[3] == 0x12 || rd.data[3] == 0x13);
        /* PB 160: the clock is PRAM's first four bytes. */
        rd = OP(r, 0x3A, 0x00, 0x04);
        CHECK_EQ(rd.data[0], 0xB0);
    } else {
        rd = OP0(r, 0x38);                    /* no clock in the PB 140 PMU */
        CHECK_EQ(rd.error, 3);
    }
    rig_free(r);
    free(r);
}

/* emulation.nvram: PRAM and the clock survive a new PMU. */
static void test_external_nvram(void) {
    rig *r = malloc(sizeof *r);
    uint8_t nvram[8192] = {0};
    rig_init(r, MC_PMU_99, MC_PMU_SHIFT, NULL, 0);
    CHECK(power_up(r)); tests++;
    mc_pmu_set_nvram(&r->pmu, nvram, sizeof nvram);
    op_result result = OP(r, 0x33, 0x12, 0x34, 0xA5);
    CHECK_EQ(result.error, 0); tests++;
    CHECK_EQ(nvram[0x1234], 0xA5); tests++;
    result = OP(r, 0x3B, 0x12, 0x34);
    CHECK_EQ(result.error, 0); tests++;
    CHECK_EQ(result.n, 1); tests++;
    CHECK_EQ(result.data[0], 0xA5); tests++;
    result = OP(r, 0x33, 0x1F, 0xFF, 0x6B);
    CHECK_EQ(result.error, 0); tests++;
    result = OP(r, 0x3B, 0x1F, 0xFF);
    CHECK_EQ(result.data[0], 0x6B); tests++;
    result = OP(r, 0x33, 0x20, 0x00, 0x7C);
    CHECK_EQ(result.error, 0); tests++;
    result = OP(r, 0x3B, 0x20, 0x00);
    CHECK_EQ(result.error, 0); tests++;
    CHECK_EQ(result.n, 1); tests++;
    CHECK_EQ(result.data[0], 0xFF); tests++;
    CHECK_EQ(nvram[0], 0); tests++;
    mc_pmu_ops.reset(&r->pmu);
    CHECK_EQ(nvram[0x1234], 0xA5); tests++;
    CHECK_EQ(nvram[0x1FFF], 0x6B); tests++;
    rig_free(r);
    free(r);
}

static void test_persistence(void) {
    tests++;
    rig *r = malloc(sizeof *r);
    rig_init(r, MC_PMU_PB160, MC_PMU_PARALLEL, NULL, 0);
    CHECK(power_up(r));
    OP(r, 0x32, 0x20, 0x02, 0x5A, 0xA5);
    OP(r, 0x30, 0xB1, 0x22, 0x33, 0x44);
    CHECK(r->pmu.dirty);
    const char *path = hw_work_path("pmu", "pram.nvram");
    const int64_t host = 3000000000LL;
    CHECK(mc_pmu_nvram_save(&r->pmu, path, host));
    rig_free(r);
    rig_init(r, MC_PMU_PB160, MC_PMU_PARALLEL, NULL, 0);
    CHECK(mc_pmu_nvram_load(&r->pmu, path, host + 100));
    CHECK(power_up(r));
    op_result rd = OP(r, 0x3A, 0x20, 0x02);
    CHECK_EQ(rd.data[0], 0x5A);
    CHECK_EQ(rd.data[1], 0xA5);
    rd = OP0(r, 0x38);
    const uint32_t s = (uint32_t)rd.data[0] << 24 | (uint32_t)rd.data[1] << 16 | (uint32_t)rd.data[2] << 8 | rd.data[3];
    CHECK(s >= 0xB1223344u + 100 && s <= 0xB1223344u + 103);
    rig_free(r);
    free(r);
}

/* The first second's battery interrupt (the charger appears), its data
 * through $78, the status through $68 and the acknowledge. */
static void test_battery_and_interrupts(unsigned model) {
    tests++;
    rig *r = malloc(sizeof *r);
    rig_init(r, model, MC_PMU_PARALLEL, NULL, 0);
    CHECK(power_up(r));
    op_result i = take_interrupt(r, MS(4000));
    CHECK_EQ(i.error, 0);
    CHECK_EQ(i.first, 0x78);
    CHECK_EQ(i.n, 4);
    CHECK_EQ(i.data[0], 0x20);              /* battery */
    CHECK_EQ(i.data[1], model == MC_PMU_PB140 ? 0x21 : 0xA3);   /* charger present, changed (PB 160: charging) */
    CHECK_EQ(i.data[2], MC_PMU_BATTERY_LEVEL);
    CHECK_EQ(i.data[3], MC_PMU_TEMPERATURE);
    op_result b = OP0(r, 0x68);
    CHECK_EQ(b.n, 3);
    /* The change acknowledged; the PB 160 firmware also sets bit 7 and
     * charges (bit 1) its first four minutes. */
    CHECK_EQ(b.data[0], model == MC_PMU_PB140 ? 0x01 : 0x83);
    const unsigned pulses = r->int_pulses;
    run_for(r, MS(200));
    CHECK_EQ(r->int_pulses, pulses);        /* nothing pending: no pulse */
    op_result none = OP0(r, 0x78);          /* nothing to read: no reply */
    CHECK(none.error != 0);
    b = OP0(r, 0x6A);                       /* the cold start's thresholds */
    CHECK_EQ(b.data[0], model == MC_PMU_PB140 ? 0x49 : 0x4E);
    CHECK_EQ(b.data[1], 0x30);
    OP(r, 0x60, 0x40, 0x31);
    b = OP0(r, 0x6A);
    CHECK_EQ(b.data[0], 0x40);
    if (model == MC_PMU_PB160) {
        /* Four minutes on: charged, trickling (bit 1 clear). */
        run_for(r, MS(241000));
        b = OP0(r, 0x68);
        CHECK_EQ(b.data[0], 0x81);
    }
    rig_free(r);
    free(r);
}

/* ADB: a Talk through $20, its answer through the interrupt; autopoll
 * reports a key; Listen; SendReset. */
static void test_adb(unsigned model, unsigned iface) {
    tests++;
    rig *r = malloc(sizeof *r);
    rig_init(r, model, iface, NULL, 0);
    CHECK(power_up(r));
    take_interrupt(r, MS(4000));            /* the battery's */
    op_result a = OP(r, 0x20, 0x2F, 0x00, 0x00);    /* Talk R3 of address 2 */
    CHECK_EQ(a.error, 0);
    op_result i = take_interrupt(r, MS(50));
    CHECK_EQ(i.error, 0);
    if (model == MC_PMU_PB140 || model == MC_PMU_PB160) {
        CHECK_EQ(i.first, 0x78);
    }
    CHECK_EQ(i.n, 4);
    CHECK_EQ(i.data[0] & 0xF0, 0x10);
    CHECK_EQ(i.data[1], 0x2F);
    CHECK_EQ(i.data[2] & 0x0F, 0x02);       /* address 2 */
    CHECK_EQ(i.data[3], 0x02);              /* handler 2 */
    /* A Talk to an empty address: the command reports no data. */
    OP(r, 0x20, 0x7C, 0x00, 0x00);
    i = take_interrupt(r, MS(50));
    CHECK_EQ(i.n, 2);
    /* Listen R3 of the mouse: handler 2 accepted. */
    OP(r, 0x20, 0x3B, 0x00, 0x02, 0x63, 0x02);
    i = take_interrupt(r, MS(50));
    CHECK_EQ(r->mouse.dev.handler, 2);
    /* Autopoll Talk R0 of the keyboard: a key comes as an interrupt. */
    OP(r, 0x20, 0x2C, 0x04, 0x00);
    take_interrupt(r, MS(50));
    run_for(r, MS(100));
    mc_adb_keyboard_key(&r->kbd, 0x00, 1);
    i = take_interrupt(r, MS(100));
    CHECK_EQ(i.error, 0);
    CHECK_EQ(i.data[0] & 0x04, 0x04);       /* autopoll */
    CHECK_EQ(i.data[1], 0x2C);
    CHECK_EQ(i.n, 4);
    CHECK_EQ(i.data[2], 0x00);
    /* Autopoll off. */
    OP0(r, 0x21);
    mc_adb_keyboard_key(&r->kbd, 0x00, 0);
    const unsigned pulses = r->int_pulses;
    run_for(r, MS(100));
    CHECK_EQ(r->int_pulses, pulses);
    rig_free(r);
    free(r);
}

/* Sleep: $7F with the signature turns power to SLEEP five ticks later; the
 * machine's wake hook brings it back (the sleep loop runs one slow-clock
 * period after power-off, sees the key, powers the CPU in reset at a tick,
 * releases it four ticks later); a bad signature is refused; $7E shuts down
 * and the power key (the wake hook when off) starts the machine again. */
static void test_sleep_and_wake(unsigned model) {
    tests++;
    rig *r = malloc(sizeof *r);
    rig_init(r, model, MC_PMU_PARALLEL, NULL, 0);
    CHECK(power_up(r));
    CHECK(!mc_pmu_wake(&r->pmu));           /* awake: nothing to do */
    op_result s = OP(r, 0x7F, 'M', 'A', 'T', 'X');
    CHECK_EQ(s.first, 0xAA);
    run_for(r, MS(100));
    CHECK_EQ(r->pmu.power, MC_PMU_POWER_ON);
    s = OP(r, 0x7F, 'M', 'A', 'T', 'T');
    CHECK_EQ(s.first, 0x70);
    run_for(r, MS(30));
    CHECK_EQ(r->pmu.power, MC_PMU_POWER_ON);    /* on its way down */
    run_for(r, MS(40));
    CHECK_EQ(r->pmu.power, MC_PMU_POWER_SLEEP);
    CHECK(mc_pmu_asleep(&r->pmu));
    CHECK_EQ(hw_fake_value(&r->fake, &r->pmu, MC_PMU_LINE_PLANES), 0xD7);
    run_for(r, MS(2000));
    CHECK_EQ(r->pmu.power, MC_PMU_POWER_SLEEP);
    const unsigned resets = hw_fake_changes(&r->fake, &r->pmu, MC_PMU_LINE_RESET);
    CHECK(mc_pmu_wake(&r->pmu));
    CHECK(!mc_pmu_wake(&r->pmu));           /* already waking */
    run_for(r, MS(60));
    CHECK_EQ(r->pmu.power, MC_PMU_POWER_SLEEP);
    run_for(r, MS(20));
    CHECK_EQ(r->pmu.power, MC_PMU_POWER_ON);
    CHECK_EQ(r->pmu.reset, 1);              /* powered, in reset */
    run_for(r, MS(50));
    CHECK_EQ(r->pmu.reset, 0);
    CHECK_EQ(hw_fake_changes(&r->fake, &r->pmu, MC_PMU_LINE_RESET), resets + 2);
    op_result v = OP0(r, 0xEA);
    CHECK_EQ(v.error, 0);
    s = OP(r, 0x7E, 'M', 'A', 'T', 'T');
    run_for(r, MS(100));
    CHECK_EQ(r->pmu.power, MC_PMU_POWER_OFF);
    CHECK(mc_pmu_wake(&r->pmu));            /* the power key */
    run_for(r, MS(1800));
    CHECK_EQ(r->pmu.power, MC_PMU_POWER_ON);
    CHECK_EQ(r->pmu.reset, 0);
    v = OP0(r, 0xEA);
    CHECK_EQ(v.error, 0);
    rig_free(r);
    free(r);
}

/* The PB 160's wakeup timer ($80): asleep, the machine wakes at the time
 * set; $82 turns it off. */
static void test_wake_timer(void) {
    tests++;
    rig *r = malloc(sizeof *r);
    rig_init(r, MC_PMU_PB160, MC_PMU_PARALLEL, NULL, 0);
    CHECK(power_up(r));
    OP(r, 0x30, 0x00, 0x00, 0x10, 0x00);
    OP(r, 0x80, 0x00, 0x00, 0x10, 0x05);
    op_result t = OP0(r, 0x88);
    CHECK_EQ(t.data[3], 0x05);
    CHECK_EQ(t.data[4], 0x01);
    OP(r, 0x7F, 'M', 'A', 'T', 'T');
    run_for(r, MS(100));
    CHECK_EQ(r->pmu.power, MC_PMU_POWER_SLEEP);
    run_for(r, MS(8000));
    CHECK_EQ(r->pmu.power, MC_PMU_POWER_ON);
    rig_free(r);
    free(r);
}

/* The new framing (PG&E, PMU99) on the shift register and on port A:
 * fixed reply lengths without the count, the version byte, the clock; the
 * PMU99's one-second interrupt. */
static void test_new_framing(unsigned model, unsigned iface) {
    tests++;
    rig *r = malloc(sizeof *r);
    rig_init(r, model, iface, NULL, 0);
    CHECK(power_up(r));
    OP(r, 0x30, 0xAB, 0xCD, 0x00, 0x01);
    op_result t = OP0(r, 0x38);
    CHECK_EQ(t.error, 0);
    CHECK_EQ(t.n, 4);
    CHECK_EQ(t.data[0], 0xAB);
    uint8_t pram[20];
    for (int i = 0; i < 20; i++) pram[i] = (uint8_t)i;
    pmgr_op(r, 0x31, pram, 20);
    op_result p = OP0(r, 0x39);
    CHECK_EQ(p.n, 20);
    CHECK(!memcmp(p.data, pram, 20));
    OP(r, 0x32, 0xF0, 0x02, 0x77, 0x88);    /* 256 bytes on the later PMUs */
    p = OP(r, 0x3A, 0xF0, 0x02);
    CHECK_EQ(p.n, 2);
    CHECK_EQ(p.data[1], 0x88);
    op_result b = OP0(r, 0x68);
    CHECK_EQ(b.n, 3);
    op_result s = OP(r, 0x7F, 'M', 'A', 'T', 'X');
    CHECK_EQ(s.first, 0xAA);
    CHECK_EQ(s.n, 0);
    op_result i = take_interrupt(r, MS(4000));
    CHECK_EQ(i.error, 0);
    if (model == MC_PMU_99) {
        /* Battery first (its bit), then the one-second interrupts. */
        i = take_interrupt(r, MS(1200));
        CHECK_EQ(i.n, 1);
        CHECK_EQ(i.data[0], 0x80);
    }
    OP(r, 0x20, 0x2F, 0x00, 0x00);
    i = take_interrupt(r, MS(50));
    CHECK_EQ(i.n, 4);
    CHECK_EQ(i.data[1], 0x2F);
    rig_free(r);
    free(r);
}

/* The 1400 table (83A21950 at $3D8B6) defines $63 as a fixed two-byte
 * read even though bit 3 of the command is clear. Follow it with a
 * battery request to check that the reply clocks did not desync. */
static void test_1400_pmu_reply(void) {
    static const uint8_t counts[512] = {[256 + 0x63] = 3, [256 + 0x68] = 4};
    tests++;
    rig *r = malloc(sizeof *r);
    rig_init(r, MC_PMU_99, MC_PMU_SHIFT, NULL, 0);
    CHECK(power_up(r));
    mc_pmu_set_counts(&r->pmu, counts);

    /* The host abandons a send while ACK is pending. Its retry must
     * start with ACK high and the abandoned byte must not be consumed. */
    r->sr = 0x68;
    r->sr_out_mode = 1;
    req(r, 0);
    CHECK_EQ(r->pmu.ack, 1);
    req(r, 1);
    run_for(r, MC_PMU_ACK_DELAY + MC_PMU_RELEASE_DELAY);
    CHECK_EQ(r->pmu.ack, 1);

    send_byte(r, 0x63);
    CHECK_EQ(recv_byte(r), 0);
    CHECK_EQ(recv_byte(r), 0);
    CHECK_EQ(r->error, 0);
    op_result b = OP0(r, 0x68);
    CHECK_EQ(b.n, 3);
    CHECK_EQ(b.error, 0);
    rig_free(r);
    free(r);
}

/* Use the original PBX ROM's command lengths and check its LCD PWM
 * conversion, calibration replies and independent brightness. */
static void test_pbx_lcd_commands(void) {
    tests++;
    rig *r = malloc(sizeof *r);
    rig_init(r, MC_PMU_99, MC_PMU_SHIFT, NULL, 0);
    mc_pmu_set_pbx_lcd(&r->pmu);
    CHECK(power_up(r));
    op_result q = OP0(r, 0x49);
    CHECK_EQ(q.error, 0);
    CHECK_EQ(q.data[0], 0x40);
    /* Finder reads the separate control before it queries calibration. */
    q = OP(r, 0x4F, 5);
    CHECK_EQ(q.error, 0);
    CHECK_EQ(q.n, 1);
    CHECK_EQ(q.data[0], 0x80);
    q = OP(r, 0x4F, 1);
    CHECK_EQ(q.error, 0);
    CHECK_EQ(q.n, 2);
    CHECK_EQ(q.data[0], 0xFF);
    CHECK_EQ(q.data[1], 0);

    CHECK_EQ(OP(r, 0x4F, 0, 0xD9, 0x6D).n, 0);
    CHECK_EQ(OP(r, 0x40, 0x10).error, 0);
    CHECK_EQ(OP(r, 0x41, 0x32).error, 0);
    q = OP(r, 0x4F, 1);
    CHECK_EQ(q.n, 2);
    CHECK_EQ(q.data[0], 0xD9);
    CHECK_EQ(q.data[1], 0x6D);
    q = OP0(r, 0x48);
    CHECK_EQ(q.data[0], 0x10);
    q = OP0(r, 0x49);
    CHECK_EQ(q.data[0], 0x32);
    q = OP(r, 0x4F, 3);
    CHECK_EQ(q.data[0], 0x8D);
    CHECK_EQ(OP(r, 0x4F, 2, 0x40).error, 0);
    q = OP(r, 0x4F, 3);
    CHECK_EQ(q.data[0], 0x6D);
    CHECK_EQ(OP(r, 0x4F, 2, 0xE0).error, 0);
    q = OP(r, 0x4F, 3);
    CHECK_EQ(q.data[0], 0xD9);
    CHECK_EQ(hw_fake_value(&r->fake, &r->pmu, MC_PMU_LINE_CONTRAST), 0xD9);
    rig_free(r);
    free(r);
}

/* --- the PG&E's boot ROM on the 68HC05 core ---------------------------------------
 * Built with the core of phase 5.1c: the boot ROM's command loop, through
 * the shift register: $E2 answers $94 (no program loaded). */
#include "pmu_pge.h"

static void test_pge_boot(const char *roms) {
    size_t size;
    uint8_t *boot = load_firmware(roms, "pmu-pge-boot", &size);
    if (!boot) return;
    tests++;
    rig *r = malloc(sizeof *r);
    rig_init(r, MC_PMU_PGE, MC_PMU_SHIFT, boot, size);
    CHECK_EQ(r->pmu.level, MC_PMU_FIRMWARE);
    run_for(r, MS(1000));                   /* its start-up delays and checksum */
    op_result s = OP0(r, 0xE2);
    CHECK_EQ(s.error, 0);
    CHECK_EQ(s.first, 0x94);
    /* The download the Duo ROMs' image ("BORG" at $AA9C0 of the Duo 210
     * ROM) is made for: $E1 with the image's first four bytes, then the
     * rest byte by byte to $FFFF; the boot ROM checks it and runs it. */
    char path[1024];
    snprintf(path, sizeof path, "%s/powerbook-duo-210.rom", roms);
    FILE *f = fopen(path, "rb");
    uint8_t *rom = malloc(1u << 20);
    if (f && rom && fread(rom, 1, 1u << 20, f) == (1u << 20) && !memcmp(rom + 0xAA9C0, "BORG", 4)) {
        const uint8_t *image = rom + 0xAA9C0;
        op_result d = pmgr_op(r, 0xE1, image, 4);
        CHECK_EQ(d.error, 0);
        unsigned i = 4;
        for (; i < 0x8004 && !r->error; i++) send_byte(r, image[i]);
        if (r->error && trace) printf("download stopped at byte %u (pc %04X)\n", i, r->pmu.pge->cpu.pc);
        CHECK_EQ(r->error, 0);
        run_for(r, MS(3000));                   /* the boot ROM's checksum, the program's first set-up */
        CHECK(r->pmu.pge->ram[0x8C - 0x40] & 0x80u);   /* the boot ROM's "program loaded" */
        op_result v = OP0(r, 0xE2);
        CHECK_EQ(v.error, 0);
        CHECK_EQ(v.first, 0x62);                /* the program runs */
    }
    if (f) fclose(f);
    free(rom);
    rig_free(r);
    free(r);
    free(boot);
}

/* --- the PG&E's main program against the protocol level ---------------------------
 * The firmware level as the Duo boards start it (the program of the Duo 210
 * ROM already in its RAM) and the protocol level answer the same: $E2, the
 * built-in keyboard's register 3 and a key through autopoll, the clock, a
 * parameter RAM byte. */
/* The interrupt that carries ADB command `cmd`'s answer (the PG&E's
 * once-a-second interrupt may come first). */
static op_result adb_answer(rig *r, uint8_t cmd) {
    op_result i;
    memset(&i, 0, sizeof i);
    i.error = -1;
    for (int k = 0; k < 4; k++) {
        i = take_interrupt(r, MS(150));
        if (!i.error && i.n >= 2 && i.data[1] == cmd) break;
    }
    return i;
}

static void pge_script(rig *r, int firmware) {
    CHECK(power_up(r));
    run_for(r, MS(firmware ? 1500 : 10));
    op_result e = OP0(r, 0xE2);
    CHECK_EQ(e.error, 0);
    CHECK_EQ(e.first, 0x62);
    op_result a = OP(r, 0x20, 0x2F, 0x00, 0x00);    /* Talk R3 of address 2 */
    CHECK_EQ(a.error, 0);
    op_result i = adb_answer(r, 0x2F);
    CHECK_EQ(i.error, 0);
    CHECK_EQ(i.n, 4);
    CHECK_EQ(i.data[2] & 0x0F, 0x02);
    /* The program's keyboard is handler $0C (its register 3, $8D6C); the
     * protocol level's is Livadix's ADB keyboard, handler 2. */
    CHECK_EQ(i.data[3], firmware ? 0x0C : 0x02);
    OP(r, 0x20, 0x2C, 0x04, 0x00);                  /* autopoll Talk R0 of the keyboard */
    take_interrupt(r, MS(100));
    run_for(r, MS(100));
    if (!mc_pmu_input_key(&r->pmu, 0x00, 1)) mc_adb_keyboard_key(&r->kbd, 0x00, 1);
    i = adb_answer(r, 0x2C);
    CHECK_EQ(i.error, 0);
    CHECK_EQ(i.data[2], 0x00);                      /* A down */
    if (!mc_pmu_input_key(&r->pmu, 0x00, 0)) mc_adb_keyboard_key(&r->kbd, 0x00, 0);
    OP0(r, 0x21);
    run_for(r, MS(100));
    mc_pmu_set_seconds(&r->pmu, 0x12345678u);
    run_for(r, MS(2100));
    op_result t = OP0(r, 0x38);
    CHECK_EQ(t.error, 0);
    const uint32_t s = (uint32_t)t.data[0] << 24 | (uint32_t)t.data[1] << 16 | (uint32_t)t.data[2] << 8 | t.data[3];
    CHECK(s >= 0x1234567Au && s <= 0x1234567Bu);
    OP(r, 0x32, 0x40, 0x01, 0xA5);                  /* PRAM byte $40 */
    uint8_t pram[256];
    mc_pmu_pram_get(&r->pmu, pram);
    CHECK_EQ(pram[0x40], 0xA5);
}

/* PG&E firmware: $7F with the signature puts the machine to sleep and
 * keeps it there; the power key wakes it (the program's charging loop on
 * the AC adapter, $24 bit 6, sees the key and restarts through its start,
 * which powers on without the battery check: pmu_pge.c), and the
 * processor runs again. */
static void pge_sleep_wake(rig *r) {
    op_result s = OP(r, 0x7F, 'M', 'A', 'T', 'T');
    CHECK_EQ(s.error, 0);
    uint64_t end = r->fake.now + MS(3000);
    while (r->pmu.power != MC_PMU_POWER_SLEEP && r->fake.now < end) run_for(r, MS(10));
    CHECK_EQ(r->pmu.power, MC_PMU_POWER_SLEEP);
    run_for(r, MS(6000));               /* past the charging loop's four-second count ($9C3C) */
    CHECK_EQ(r->pmu.power, MC_PMU_POWER_SLEEP);
    /* Hit the charging loop's masked scan window, as an arbitrary host
     * key press can. $90C3 clears the key interrupt enable before scanning
     * columns; $C9AC enables it again. The held power key must survive it. */
    end = r->fake.now + MS(1000);
    while (r->pmu.pge->cpu.pc != 0x90C5u && r->fake.now < end) run_for(r, 1);
    CHECK_EQ(r->pmu.pge->cpu.pc, 0x90C5u);
    CHECK(!(r->pmu.pge->reg[0x0F] & 8u));
    mc_pmu_input_key(&r->pmu, 0x7F, 1);
    run_for(r, MS(300));
    mc_pmu_input_key(&r->pmu, 0x7F, 0);
    end = r->fake.now + MS(4000);
    while (!(r->pmu.power == MC_PMU_POWER_ON && !r->pmu.reset) && r->fake.now < end) run_for(r, MS(10));
    CHECK_EQ(r->pmu.power, MC_PMU_POWER_ON);
    CHECK_EQ(r->pmu.reset, 0);
}

static void test_pge_program(const char *roms) {
    size_t size;
    uint8_t *boot = load_firmware(roms, "pmu-pge-boot", &size);
    char path[1024];
    snprintf(path, sizeof path, "%s/powerbook-duo-210.rom", roms);
    FILE *f = fopen(path, "rb");
    uint8_t *rom = malloc(1u << 20);
    if (boot && f && rom && fread(rom, 1, 1u << 20, f) == (1u << 20) && !memcmp(rom + 0xAA9C0, "BORG", 4)) {
        tests++;
        rig *r = malloc(sizeof *r);
        rig_init(r, MC_PMU_PGE, MC_PMU_SHIFT, NULL, 0);
        pge_script(r, 0);
        rig_free(r);
        rig_init(r, MC_PMU_PGE, MC_PMU_SHIFT, boot, size);
        mc_pmu_pge_program(&r->pmu, rom + 0xAA9C0, 0x8004);
        r->pmu.dev.ops->reset(&r->pmu);
        pge_script(r, 1);
        pge_sleep_wake(r);
        rig_free(r);
        free(r);
    }
    if (f) fclose(f);
    free(rom);
    free(boot);
}

/* The Portable's PMU at the protocol level (pmu.h): main power at the
 * first tick of the board's 60.15 Hz, the processor at the second; the
 * cold start's flag and the battery in $78's sources, the ADB data through
 * $28; the one-second pulses; sleep (the processor in reset) and wake. */
static void test_portable(void) {
    tests++;
    rig *r = malloc(sizeof *r);
    rig_init(r, MC_PMU_PORTABLE, MC_PMU_PARALLEL, NULL, 0);
    CHECK_EQ(r->pmu.power, MC_PMU_POWER_OFF);
    run_for(r, MS(10));
    CHECK_EQ(r->pmu.power, MC_PMU_POWER_ON);
    CHECK_EQ(r->pmu.reset, 1);
    run_for(r, MS(20));
    CHECK_EQ(r->pmu.reset, 0);
    CHECK_EQ(hw_fake_value(&r->fake, &r->pmu, MC_PMU_LINE_PLANES), 0x48);
    op_result v = OP0(r, 0xEA);
    CHECK_EQ(v.n, 2);
    CHECK_EQ(v.data[0], 0x02);
    CHECK_EQ(v.data[1], 0xB5);
    op_result i = take_interrupt(r, MS(6000));
    CHECK_EQ(i.n, 1);
    CHECK_EQ(i.data[0], 0x0A);                  /* the cold start and the battery */
    op_result b = OP0(r, 0x68);
    CHECK_EQ(b.n, 3);
    CHECK_EQ(b.data[0], 0x23);                  /* charger, charging, changed */
    CHECK_EQ(b.data[1], MC_PMU_BATTERY_LEVEL);
    const unsigned seconds = hw_fake_changes(&r->fake, &r->pmu, MC_PMU_LINE_SECOND);
    run_for(r, MS(3000));
    const unsigned pulses = (hw_fake_changes(&r->fake, &r->pmu, MC_PMU_LINE_SECOND) - seconds) / 2;
    CHECK(pulses >= 2 && pulses <= 3);
    b = OP0(r, 0x68);
    CHECK_EQ(b.data[0], 0x01);                  /* charged */
    OP(r, 0x20, 0x2F, 0x00, 0x00);
    i = take_interrupt(r, MS(50));
    CHECK_EQ(i.data[0], 0x01);
    op_result a = OP0(r, 0x28);
    CHECK_EQ(a.n, 5);
    CHECK_EQ(a.data[0], 0x2F);
    CHECK_EQ(a.data[2], 2);
    CHECK_EQ(a.data[3], 0x62);                  /* the keyboard's register 3 */
    /* The programmer's reset button (INT2): the processor in reset while it
     * is held, let go at a tick after its release; the PMU answers again. */
    CHECK(mc_pmu_reset_switch(&r->pmu, 1));
    run_for(r, MS(100));
    CHECK_EQ(r->pmu.reset, 1);
    CHECK_EQ(r->pmu.power, MC_PMU_POWER_ON);
    mc_pmu_reset_switch(&r->pmu, 0);
    run_for(r, MS(40));
    CHECK_EQ(r->pmu.reset, 0);
    v = OP0(r, 0xEA);
    CHECK(v.n == 2 && v.data[1] == 0xB5);
    op_result s7 = OP(r, 0x7F, 'M', 'A', 'T', 'T');
    CHECK_EQ(s7.first, 0x70);
    run_for(r, MS(60));
    CHECK_EQ(r->pmu.power, MC_PMU_POWER_SLEEP);
    CHECK_EQ(r->pmu.reset, 1);
    CHECK(mc_pmu_asleep(&r->pmu));
    CHECK(mc_pmu_wake(&r->pmu));
    run_for(r, MS(200));
    CHECK_EQ(r->pmu.power, MC_PMU_POWER_ON);
    CHECK_EQ(r->pmu.reset, 0);
    rig_free(r);
    free(r);
}

/* --- the differential --------------------------------------------------------------- */


/* One script, run on a rig: the replies and interrupt data go to `log`. */
typedef struct transcript {
    uint8_t bytes[4096];
    unsigned n;
    uint64_t t_power, t_first_int;
    uint64_t dur[128];              /* each operation's duration, C15M */
    uint64_t tol[128];              /* the difference allowed */
    char name[128][16];
    unsigned nops;
    uint64_t t_op;
    uint64_t t_sleep, t_wake, t_wake_run, t_off, t_on;
} transcript;

static rig *log_rig;

/* The timing tolerance of the next logged operation (C15M): byte
 * handshakes and fixed-length work within 0.3 ms or 20%; events the PMU's
 * tick paces (autopoll, the first second) within a tick and a poll. */
static uint64_t next_tol;
#define TOL_TICK ((uint64_t)MC_PMU_TICK + (uint64_t)MC_C15M_HZ * 5u / 1000u)

static void log_op(transcript *tr, const op_result *o) {
    if (tr->nops < 128) {
        tr->dur[tr->nops] = log_rig->fake.now - tr->t_op;
        tr->tol[tr->nops] = next_tol;
        next_tol = 0;
        snprintf(tr->name[tr->nops], 16, "%02X/%u", o->first, o->n);
        tr->nops++;
    }
    tr->t_op = log_rig->fake.now;
    if (tr->n + 4 + o->n > sizeof tr->bytes) return;
    tr->bytes[tr->n++] = (uint8_t)o->error;
    tr->bytes[tr->n++] = o->first;
    tr->bytes[tr->n++] = (uint8_t)o->n;
    memcpy(tr->bytes + tr->n, o->data, o->n);
    tr->n += o->n;
}

static void dump(const char *label, const op_result *o) {
    if (!trace) return;
    printf("  %s: err %d first %02X n %u:", label, o->error, o->first, o->n);
    for (unsigned i = 0; i < o->n; i++) printf(" %02X", o->data[i]);
    printf("   (wait %.0f) [", int_wait);
    int_wait = 0;
    for (unsigned i = 0; i < nbyte_lat; i++) printf(" %.0f", byte_lat[i]);
    printf("]\n");
}

/* Runs until `cond` holds or `limit` passes; returns the time taken. */
static uint64_t until_power(rig *r, int power, int reset, uint64_t limit) {
    const uint64_t t0 = r->fake.now;
    while (!(r->pmu.power == power && r->pmu.reset == reset) && r->fake.now - t0 < limit) run_for(r, US(100));
    return r->fake.now - t0;
}

static void script(rig *r, transcript *tr) {
    memset(tr, 0, sizeof *tr);
    log_rig = r;
    power_up(r);
    tr->t_power = r->fake.now;
    tr->t_op = r->fake.now;
    op_result o;
    o = take_interrupt(r, MS(4000));
    tr->t_first_int = r->fake.now - tr->t_power;
    next_tol = TOL_TICK;
    dump("int (battery)", &o); log_op(tr, &o);
    o = OP0(r, 0xEA); dump("$EA", &o); log_op(tr, &o);
    o = OP0(r, 0x18); dump("$18", &o); log_op(tr, &o);
    o = OP(r, 0x10, 0x84); dump("$10", &o); log_op(tr, &o);
    o = OP0(r, 0x18); dump("$18", &o); log_op(tr, &o);
    o = OP(r, 0x40, 0x0C); dump("$40", &o); log_op(tr, &o);
    o = OP0(r, 0x48); dump("$48", &o); log_op(tr, &o);
    o = OP(r, 0x50, 0x05); dump("$50", &o); log_op(tr, &o);
    o = OP0(r, 0x58); dump("$58", &o); log_op(tr, &o);
    o = OP(r, 0x90, 0x01); dump("$90", &o); log_op(tr, &o);
    o = OP0(r, 0x98); dump("$98", &o); log_op(tr, &o);
    o = OP0(r, 0x68); dump("$68", &o); log_op(tr, &o);
    o = OP0(r, 0x69); dump("$69", &o); log_op(tr, &o);
    o = OP0(r, 0x6A); dump("$6A", &o); log_op(tr, &o);
    o = OP(r, 0x60, 0x40, 0x31); dump("$60", &o); log_op(tr, &o);
    o = OP0(r, 0x6A); dump("$6A", &o); log_op(tr, &o);
    o = OP(r, 0xD8, 0x05); dump("$D8", &o); log_op(tr, &o);
    reply_timeout = MS(1000);              /* the self-test runs the ROM and RAM tests */
    o = OP0(r, 0xEC); dump("$EC", &o); log_op(tr, &o);
    reply_timeout = MS(32);
    uint8_t pram[20];
    for (int i = 0; i < 20; i++) pram[i] = (uint8_t)(0x31 * i);
    o = pmgr_op(r, 0x31, pram, 20); dump("$31", &o); log_op(tr, &o);
    next_tol = TOL_TICK;                   /* the firmware may be in its tick's work */
    o = OP0(r, 0x39); dump("$39", &o); log_op(tr, &o);
    o = OP(r, 0x32, 0x50, 0x04, 1, 2, 3, 4); dump("$32", &o); log_op(tr, &o);
    next_tol = TOL_TICK;
    o = OP(r, 0x3A, 0x4E, 0x08); dump("$3A", &o); log_op(tr, &o);
    o = OP(r, 0xE8, 0x00, 0x80, 0x04); dump("$E8", &o); log_op(tr, &o);
    if (r->pmu.model == MC_PMU_PB160) {
        o = OP(r, 0x30, 0xB0, 0x01, 0x02, 0x03); dump("$30", &o); log_op(tr, &o);
        o = OP0(r, 0x38); dump("$38", &o); log_op(tr, &o);
        o = OP(r, 0x80, 0xB0, 0x01, 0x02, 0x40); dump("$80", &o); log_op(tr, &o);
        o = OP0(r, 0x88); dump("$88", &o); log_op(tr, &o);
        o = OP0(r, 0x82); dump("$82", &o); log_op(tr, &o);
        o = OP0(r, 0x88); dump("$88", &o); log_op(tr, &o);
    }
    /* ADB: Talk R3 of each default address, a Listen, a Talk R0 with a key,
     * autopoll and a key, a Service Request while polling, SendReset. */
    static const uint8_t adb[] = {0x2F, 0x3F, 0x4F, 0x7C};
    for (unsigned k = 0; k < sizeof adb; k++) {
        o = OP(r, 0x20, adb[k], 0x00, 0x00); log_op(tr, &o);
        o = take_interrupt(r, MS(50)); dump("ADB", &o); log_op(tr, &o);
    }
    o = OP(r, 0x20, 0x3B, 0x00, 0x02, 0x63, 0x02); log_op(tr, &o);
    o = take_interrupt(r, MS(50)); dump("ADB listen", &o); log_op(tr, &o);
    mc_adb_keyboard_key(&r->kbd, 0x0E, 1);
    o = OP(r, 0x20, 0x2C, 0x00, 0x00); log_op(tr, &o);
    o = take_interrupt(r, MS(50)); dump("ADB key", &o); log_op(tr, &o);
    o = OP(r, 0x20, 0x2C, 0x04, 0x00); log_op(tr, &o);
    o = take_interrupt(r, MS(50)); dump("ADB poll", &o); log_op(tr, &o);
    run_for(r, MS(60));
    mc_adb_keyboard_key(&r->kbd, 0x0E, 0);
    o = take_interrupt(r, MS(100));
    next_tol = TOL_TICK;
    dump("ADB poll key", &o); log_op(tr, &o);
    mc_adb_mouse_move(&r->mouse, 3, -2, 1);
    o = take_interrupt(r, MS(100));
    next_tol = TOL_TICK;
    dump("ADB poll SRQ", &o); log_op(tr, &o);
    o = OP0(r, 0x21); log_op(tr, &o);
    o = OP(r, 0x20, 0x00, 0x00, 0x00); log_op(tr, &o);
    o = take_interrupt(r, MS(50)); dump("ADB reset", &o); log_op(tr, &o);
    /* Sleep and wake: the times to power off, back on, and through the
     * CPU's reset pulse. */
    o = OP(r, 0x7F, 'M', 'A', 'T', 'T'); dump("$7F", &o); log_op(tr, &o);
    tr->t_sleep = until_power(r, MC_PMU_POWER_SLEEP, 0, MS(500));
    run_for(r, MS(300));
    tr->bytes[tr->n++] = r->pmu.power;
    mc_pmu_wake(&r->pmu);
    tr->t_wake = until_power(r, MC_PMU_POWER_ON, 1, MS(4000));      /* the CPU powered, in reset */
    tr->t_wake_run = until_power(r, MC_PMU_POWER_ON, 0, MS(4000));  /* and running */
    tr->bytes[tr->n++] = r->pmu.power;
    tr->bytes[tr->n++] = r->pmu.reset;
    tr->t_op = r->fake.now;
    o = OP0(r, 0xEA); dump("$EA after wake", &o); log_op(tr, &o);
    /* Shut down ($7E) and the power key. */
    o = OP(r, 0x7E, 'M', 'A', 'T', 'T'); dump("$7E", &o); log_op(tr, &o);
    tr->t_off = until_power(r, MC_PMU_POWER_OFF, 0, MS(500));
    tr->bytes[tr->n++] = r->pmu.power;
    mc_pmu_wake(&r->pmu);
    tr->t_on = until_power(r, MC_PMU_POWER_ON, 0, MS(6000));
    tr->bytes[tr->n++] = r->pmu.power;
    run_for(r, MS(200));
    tr->t_op = r->fake.now;
    o = OP0(r, 0xEA); dump("$EA after power key", &o); log_op(tr, &o);
}


/* The Portable's firmware: its own command set (pmu.h), its tick from the
 * board's 60.15 Hz, $78 the interrupt's sources and $28 the ADB data, no
 * $60, $6A, $D8 or $7E. */
static void script_portable(rig *r, transcript *tr) {
    memset(tr, 0, sizeof *tr);
    log_rig = r;
    power_up(r);
    tr->t_power = r->fake.now;
    tr->t_op = r->fake.now;
    op_result o;
    o = take_interrupt(r, MS(6000));
    tr->t_first_int = r->fake.now - tr->t_power;
    next_tol = TOL_TICK;
    dump("int (battery)", &o); log_op(tr, &o);
    next_tol = TOL_TICK;
    o = OP0(r, 0x68); dump("$68", &o); log_op(tr, &o);
    next_tol = TOL_TICK;
    o = OP0(r, 0xEA); dump("$EA", &o); log_op(tr, &o);
    o = OP0(r, 0x18); dump("$18", &o); log_op(tr, &o);
    o = OP(r, 0x10, 0x84); dump("$10", &o); log_op(tr, &o);
    o = OP0(r, 0x18); dump("$18", &o); log_op(tr, &o);
    o = OP(r, 0x40, 0x0C); dump("$40", &o); log_op(tr, &o);
    o = OP0(r, 0x48); dump("$48", &o); log_op(tr, &o);
    o = OP(r, 0x50, 0x07); dump("$50", &o); log_op(tr, &o);
    o = OP0(r, 0x58); dump("$58", &o); log_op(tr, &o);
    o = OP(r, 0x90, 0x01); dump("$90", &o); log_op(tr, &o);
    o = OP0(r, 0x98); dump("$98", &o); log_op(tr, &o);
    run_for(r, MS(3000));                  /* the charger's machine settles */
    tr->t_op = r->fake.now;
    next_tol = TOL_TICK;
    o = OP0(r, 0x68); dump("$68", &o); log_op(tr, &o);
    next_tol = TOL_TICK;
    o = OP0(r, 0x69); dump("$69", &o); log_op(tr, &o);
    reply_timeout = MS(1000);              /* the self-test runs the ROM and RAM tests */
    next_tol = TOL_TICK;
    o = OP0(r, 0xEC); dump("$EC", &o); log_op(tr, &o);
    reply_timeout = MS(32);
    uint8_t pram[20];
    for (int i = 0; i < 20; i++) pram[i] = (uint8_t)(0x31 * i);
    next_tol = TOL_TICK;
    o = pmgr_op(r, 0x31, pram, 20); dump("$31", &o); log_op(tr, &o);
    next_tol = TOL_TICK;
    o = OP0(r, 0x39); dump("$39", &o); log_op(tr, &o);
    next_tol = TOL_TICK;
    o = OP(r, 0x32, 0x50, 0x04, 1, 2, 3, 4); dump("$32", &o); log_op(tr, &o);
    next_tol = TOL_TICK;
    o = OP(r, 0x3A, 0x4E, 0x08); dump("$3A", &o); log_op(tr, &o);
    next_tol = TOL_TICK;
    o = OP(r, 0xE8, 0x00, 0x80, 0x04); dump("$E8", &o); log_op(tr, &o);
    next_tol = TOL_TICK;
    o = OP(r, 0x30, 0xB0, 0x01, 0x02, 0x03); dump("$30", &o); log_op(tr, &o);
    next_tol = TOL_TICK;
    o = OP0(r, 0x38); dump("$38", &o); log_op(tr, &o);
    next_tol = TOL_TICK;
    o = OP(r, 0x80, 0xB0, 0x01, 0x02, 0x40); dump("$80", &o); log_op(tr, &o);
    next_tol = TOL_TICK;
    o = OP0(r, 0x88); dump("$88", &o); log_op(tr, &o);
    next_tol = TOL_TICK;
    o = OP0(r, 0x82); dump("$82", &o); log_op(tr, &o);
    next_tol = TOL_TICK;
    o = OP0(r, 0x88); dump("$88", &o); log_op(tr, &o);
    /* ADB, each transaction's interrupt answered with $78 and $28. */
    static const uint8_t adb[] = {0x2F, 0x3F, 0x4F, 0x7C};
    for (unsigned k = 0; k < sizeof adb; k++) {
        next_tol = TOL_TICK;
        o = OP(r, 0x20, adb[k], 0x00, 0x00); log_op(tr, &o);
        next_tol = TOL_TICK;
        o = take_interrupt(r, MS(50)); dump("ADB int", &o); log_op(tr, &o);
        next_tol = TOL_TICK;
        o = OP0(r, 0x28); dump("ADB", &o); log_op(tr, &o);
    }
    next_tol = TOL_TICK;
    o = OP(r, 0x20, 0x3B, 0x00, 0x02, 0x63, 0x02); log_op(tr, &o);
    next_tol = TOL_TICK;
    o = take_interrupt(r, MS(50)); log_op(tr, &o);
    next_tol = TOL_TICK;
    o = OP0(r, 0x28); dump("ADB listen", &o); log_op(tr, &o);
    mc_adb_keyboard_key(&r->kbd, 0x0E, 1);
    next_tol = TOL_TICK;
    o = OP(r, 0x20, 0x2C, 0x00, 0x00); log_op(tr, &o);
    next_tol = TOL_TICK;
    o = take_interrupt(r, MS(50)); log_op(tr, &o);
    next_tol = TOL_TICK;
    o = OP0(r, 0x28); dump("ADB key", &o); log_op(tr, &o);
    next_tol = TOL_TICK;
    o = OP(r, 0x20, 0x2C, 0x04, 0x00); log_op(tr, &o);
    next_tol = TOL_TICK;
    o = take_interrupt(r, MS(50)); log_op(tr, &o);
    next_tol = TOL_TICK;
    o = OP0(r, 0x28); dump("ADB poll", &o); log_op(tr, &o);
    run_for(r, MS(60));
    mc_adb_keyboard_key(&r->kbd, 0x0E, 0);
    o = take_interrupt(r, MS(100));
    next_tol = 2 * TOL_TICK;
    log_op(tr, &o);
    next_tol = TOL_TICK;
    o = OP0(r, 0x28); dump("ADB poll key", &o); log_op(tr, &o);
    next_tol = TOL_TICK;
    o = OP0(r, 0x21); log_op(tr, &o);
    next_tol = TOL_TICK;
    o = OP(r, 0x20, 0x00, 0x00, 0x00); log_op(tr, &o);
    next_tol = TOL_TICK;
    o = take_interrupt(r, MS(50)); log_op(tr, &o);
    next_tol = TOL_TICK;
    o = OP0(r, 0x28); dump("ADB reset", &o); log_op(tr, &o);
    /* Sleep and wake: the processor in reset and the power off, then the
     * key: powered, in reset, running. */
    next_tol = TOL_TICK;
    o = OP(r, 0x7F, 'M', 'A', 'T', 'T'); dump("$7F", &o); log_op(tr, &o);
    tr->t_sleep = until_power(r, MC_PMU_POWER_SLEEP, 1, MS(500));
    run_for(r, MS(300));
    tr->bytes[tr->n++] = r->pmu.power;
    mc_pmu_wake(&r->pmu);
    tr->t_wake = until_power(r, MC_PMU_POWER_ON, 1, MS(4000));
    tr->t_wake_run = until_power(r, MC_PMU_POWER_ON, 0, MS(4000));
    tr->bytes[tr->n++] = r->pmu.power;
    tr->bytes[tr->n++] = r->pmu.reset;
    tr->t_op = r->fake.now;
    next_tol = TOL_TICK;
    o = OP0(r, 0xEA); dump("$EA after wake", &o); log_op(tr, &o);
}

/* Handshake latencies from a rig's events, in microseconds (medians and
 * worst cases): [0] REQ low to ACK low (a byte to the PMU), [1] REQ high to
 * ACK high (its end), [2] REQ low to ACK high (a byte from the PMU), [3] REQ
 * high to the next byte's ACK low. */
static int cmp_double(const void *a, const void *b) {
    const double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

static void latencies(const rig *r, double median[4], double worst[4]) {
    static double v[4][8192];
    unsigned n[4] = {0};
    int ack = 1, waiting = -1;
    uint64_t since = 0;
    for (unsigned i = 0; i < r->nev; i++) {
        const event *e = &r->ev[i];
        if (e->line == -1) {
            if (e->state == 0) waiting = ack ? 0 : 2;
            else waiting = ack ? 3 : 1;
            since = e->t;
        } else if (e->line == MC_PMU_LINE_ACK) {
            ack = e->state;
            const int kind = waiting;
            if (kind >= 0 && ((kind == 0 || kind == 3) ? !ack : ack)) {
                const double us = (double)(e->t - since) * 1e6 / MC_C15M_HZ;
                if ((kind != 3 || us < 1000.0) && n[kind] < 8192) v[kind][n[kind]++] = us;
                waiting = -1;
            }
        }
    }
    for (int k = 0; k < 4; k++) {
        qsort(v[k], n[k], sizeof v[k][0], cmp_double);
        median[k] = n[k] ? v[k][n[k] / 2] : 0;
        worst[k] = n[k] ? v[k][n[k] - 1] : 0;
    }
}

static unsigned differential(const char *roms) {
    unsigned compared = 0;
    static const unsigned models[] = {MC_PMU_PB140, MC_PMU_PB160, MC_PMU_PORTABLE};
    for (size_t m = 0; m < 3; m++) {
        static const char *const KEYS[] = {"pmu-pb140", "pmu-pb160", "pmu-portable"};
        size_t size;
        uint8_t *fw = load_firmware(roms, KEYS[m], &size);
        if (!fw) continue;
        test_select(roms, models[m], KEYS[m]);
        const mc_firmware *id = mc_firmware_identify(fw, size);
        CHECK(id != NULL);
        if (!id) continue;
        tests++;
        rig *a = malloc(sizeof *a), *b = malloc(sizeof *b);
        transcript *ta = malloc(sizeof *ta), *tb = malloc(sizeof *tb);
        if (trace) printf("%s, protocol level:\n", id->file);
        rig_init(a, models[m], MC_PMU_PARALLEL, NULL, 0);
        (models[m] == MC_PMU_PORTABLE ? script_portable : script)(a, ta);
        if (trace) printf("%s, firmware level:\n", id->file);
        rig_init(b, models[m], MC_PMU_PARALLEL, fw, id->size);
        CHECK_EQ(b->pmu.level, MC_PMU_FIRMWARE);
        (models[m] == MC_PMU_PORTABLE ? script_portable : script)(b, tb);
        if (ta->n != tb->n || memcmp(ta->bytes, tb->bytes, ta->n)) {
            fprintf(stderr, "%s: the protocol level's byte stream differs from the firmware's\n", id->file);
            for (unsigned i = 0; i < ta->n || i < tb->n; i++)
                if (i >= ta->n || i >= tb->n || ta->bytes[i] != tb->bytes[i]) {
                    fprintf(stderr, "  first difference at %u: %02X vs %02X\n", i, i < ta->n ? ta->bytes[i] : 0,
                            i < tb->n ? tb->bytes[i] : 0);
                    break;
                }
            mc_test_failures++;
        }
        /* Timing: every operation within its tolerance, the handshake's
         * median latencies within 10 us, power events within two ticks
         * (and four passes of the slow sleep loop for the wakes). */
        for (unsigned k = 0; k < ta->nops && k < tb->nops; k++) {
            const uint64_t x = ta->dur[k], y = tb->dur[k];
            const uint64_t d = x > y ? x - y : y - x;
            uint64_t tol = tb->tol[k] ? tb->tol[k] : US(300);
            if (!tb->tol[k] && y / 5 > tol) tol = y / 5;
            if (trace)
                printf("  op %2u %-8s %9.3f ms  %9.3f ms%s\n", k, tb->name[k], x * 1000.0 / MC_C15M_HZ,
                       y * 1000.0 / MC_C15M_HZ, d > tol ? "  <- out of tolerance" : "");
            if (d > tol) {
                fprintf(stderr, "%s: operation %u (%s) takes %.3f ms, the firmware %.3f ms\n", id->file, k, tb->name[k],
                        x * 1000.0 / MC_C15M_HZ, y * 1000.0 / MC_C15M_HZ);
                mc_test_failures++;
            }
        }
        double la[4], lb[4], wa[4], wb[4];
        latencies(a, la, wa);
        latencies(b, lb, wb);
        for (int k = 0; k < 4; k++) {
            if (trace) printf("  latency %d: %.1f us (worst %.1f) / %.1f us (worst %.1f)\n", k, la[k], wa[k], lb[k], wb[k]);
            if (la[k] - lb[k] > 10.0 || lb[k] - la[k] > 10.0) {
                fprintf(stderr, "%s: handshake latency %d %.1f us, the firmware's %.1f us\n", id->file, k, la[k], lb[k]);
                mc_test_failures++;
            }
        }
        const uint64_t ticks2 = 2u * (uint64_t)MC_PMU_TICK;
        const uint64_t pa[] = {ta->t_power, ta->t_first_int, ta->t_sleep, ta->t_wake, ta->t_wake_run, ta->t_off, ta->t_on};
        const uint64_t pb[] = {tb->t_power, tb->t_first_int, tb->t_sleep, tb->t_wake, tb->t_wake_run, tb->t_off, tb->t_on};
        static const char *const pn[] = {"power-up", "first second", "sleep", "wake", "wake run", "shut down", "power key"};
        /* The slow loop's pass (16.6 ms) adds its phase to the wakes. */
        static const unsigned passes[] = {0, 0, 0, 4, 0, 0, 4};
        for (int k = 0; k < 7; k++) {
            const uint64_t d = pa[k] > pb[k] ? pa[k] - pb[k] : pb[k] - pa[k];
            const uint64_t tolerance = ticks2 + passes[k] * US(16602);
            if (trace) printf("  %-12s %9.1f ms  %9.1f ms\n", pn[k], pa[k] * 1000.0 / MC_C15M_HZ, pb[k] * 1000.0 / MC_C15M_HZ);
            if (d > tolerance) {
                fprintf(stderr, "%s: %s after %.1f ms, the firmware's %.1f ms\n", id->file, pn[k], pa[k] * 1000.0 / MC_C15M_HZ,
                        pb[k] * 1000.0 / MC_C15M_HZ);
                mc_test_failures++;
            }
        }
        compared++;
        rig_free(a);
        rig_free(b);
        free(a); free(b); free(ta); free(tb);
        free(fw);
    }
    return compared;
}

int main(int argc, char **argv) {
    if (argc >= 3 && !strcmp(argv[1], "--pge")) {
        trace = 1;
        test_pge_boot(argv[2]);
        test_pge_program(argv[2]);
        printf("test_pmu --pge: %d failure(s)\n", mc_test_failures);
        return mc_test_failures ? 1 : 0;
    }
    if (argc >= 3 && (!strcmp(argv[1], "--diff") || !strcmp(argv[1], "--trace"))) {
        trace = !strcmp(argv[1], "--trace");
        const unsigned n = differential(argv[2]);
        printf("test_pmu %s: %u firmware(s) compared, %d failure(s)\n", argv[1], n, mc_test_failures);
        if (!n) return 77;
        return mc_test_failures ? 1 : 0;
    }
    static const unsigned models[] = {MC_PMU_PB140, MC_PMU_PB160};
    for (size_t m = 0; m < 2; m++) {
        test_power_up_and_version(models[m]);
        test_pram_and_clock(models[m]);
        test_battery_and_interrupts(models[m]);
        test_adb(models[m], MC_PMU_PARALLEL);
        test_sleep_and_wake(models[m]);
    }
    test_power_planes_contrast_modem();
    test_portable();
    test_wake_timer();
    test_external_nvram();
    test_persistence();
    test_new_framing(MC_PMU_PGE, MC_PMU_SHIFT);
    test_new_framing(MC_PMU_99, MC_PMU_PARALLEL);
    test_new_framing(MC_PMU_99, MC_PMU_SHIFT);
    test_1400_pmu_reply();
    test_pbx_lcd_commands();
    test_adb(MC_PMU_PGE, MC_PMU_SHIFT);
    test_pge_boot(MC_ROMS);
    test_pge_program(MC_ROMS);
    printf("test_pmu: %d tests, %d failure(s)\n", tests, mc_test_failures);
    return mc_test_failures ? 1 : 0;
}
