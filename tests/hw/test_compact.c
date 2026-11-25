/* test_compact: the Macintosh 128K, 512K, 512Ke and Plus: the M0110 and
 * M0110A keyboards on a VIA's shift register (m0110.h), the quadrature
 * mouse (quadmouse.h), the video and sound buffers (video/compactvideo.h)
 * and the boards (machines/compact.c).
 *
 * Livadix's own tests, from the Guide to the Macintosh Family Hardware, 2nd
 * ed. (chapters 3, 4 and 12-14, as the headers cite them): the keyboard's
 * command clocked out of the VIA in mode 7 and its reply into it in mode
 * 3; Inquiry answering Null after a quarter of a second, or the next key
 * transition at once; Instant, Model Number and Test; the transition
 * bytes (code << 1 | 1, bit 7 up) and the keypad's prefix $79; the
 * mouse's X1/Y1 edges with X2/Y2 set before them for each direction, and
 * a step held while its axis is not ready; vertical blanking at line 342
 * of 370, 704 dot clocks a line; the buffers at the top of RAM, PA6 and
 * PA3 selecting the main ones; a frame's 370 samples from the sound
 * buffer's high bytes at the volume, PB7 high silencing them. The boards:
 * the overlay after reset (the ROM at 0, the RAM at $60 0000) released by
 * PA4; the RAM repeating through 4 MB; the Plus's ROM in $40 0000-$43
 * FFFF only; the SCC read at even and written at odd addresses; the VIA on
 * the high byte; no bus error anywhere. */
#include "hw_machine.h"
#include "hw_test.h"
#include "m0110.h"
#include "quadmouse.h"
#include "scc.h"
#include "via.h"
#include "video/compactvideo.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define US(n) ((uint64_t)(n) * MC_C15M_HZ / 1000000u)

static hw_fake fake;
static mc_via via;
static mc_m0110 kb;

static uint8_t via_read(unsigned reg) { return (uint8_t)mc_via_ops.read(&via, reg << 9, 1); }
static void via_write(unsigned reg, uint8_t v) {
    mc_via_ops.write(&via, reg << 9, 1, v);
    if (reg == MC_VIA_SR) mc_m0110_sr_written(&kb);
}

static void keyboard(int model) {
    hw_fake_init(&fake);
    mc_via_init(&via, &fake.host, "VIA", 9);
    mc_via_ops.reset(&via);
    mc_m0110_init(&kb, &fake.host, "Keyboard", &via, model);
    mc_m0110_ops.reset(&kb);
    via_write(MC_VIA_IER, 0x7F);
}

/* A command as the ROM sends it (ROM 4D1F8172 at $25A4): mode 7, the
 * command in the shift register; the keyboard clocks it out. Then mode 3
 * and the reply, after `wait` microseconds; -1 when none came. */
static int exchange(uint8_t command, unsigned wait) {
    via_write(MC_VIA_ACR, 0x1C);
    via_write(MC_VIA_SR, command);
    hw_fake_step(&fake, US(3000));
    if (!(via_read(MC_VIA_IFR) & MC_VIA_IRQ_SR)) return -2;     /* not clocked out */
    CHECK_EQ(kb.last_command, command);
    via_write(MC_VIA_IFR, MC_VIA_IRQ_SR);
    via_write(MC_VIA_ACR, 0x0C);
    via_read(MC_VIA_SR);
    hw_fake_step(&fake, US(wait));
    if (!(via_read(MC_VIA_IFR) & MC_VIA_IRQ_SR)) return -1;
    return via_read(MC_VIA_SR);
}

static void test_keyboard(void) {
    keyboard(MC_M0110A);
    /* Model Number: the M0110A's byte; Test: ACK; an unknown command: NAK. */
    CHECK_EQ(exchange(MC_M0110_MODEL, 3000), 0x0B);
    CHECK_EQ(exchange(MC_M0110_TEST, 3000), MC_M0110_ACK);
    CHECK_EQ(exchange(0x55, 3000), MC_M0110_NAK);
    /* Inquiry with no key: nothing for a quarter of a second, then Null. */
    CHECK_EQ(exchange(MC_M0110_INQUIRY, 200000), -1);
    hw_fake_step(&fake, US(60000));
    CHECK(via_read(MC_VIA_IFR) & MC_VIA_IRQ_SR);
    CHECK_EQ(via_read(MC_VIA_SR), MC_M0110_NULL);
    /* Instant with no key: Null at once. */
    CHECK_EQ(exchange(MC_M0110_INSTANT, 3000), MC_M0110_NULL);
    /* A key during an Inquiry: its transition comes at once. 'S' (code 1)
     * down $03, up $83. */
    via_write(MC_VIA_ACR, 0x1C);
    via_write(MC_VIA_SR, MC_M0110_INQUIRY);
    hw_fake_step(&fake, US(3000));
    via_write(MC_VIA_ACR, 0x0C);
    via_read(MC_VIA_SR);
    hw_fake_step(&fake, US(50000));
    CHECK(!(via_read(MC_VIA_IFR) & MC_VIA_IRQ_SR));
    CHECK(mc_m0110_key(&kb, 0x01, 1));
    hw_fake_step(&fake, US(3000));
    CHECK_EQ(via_read(MC_VIA_SR), 0x03);
    CHECK(mc_m0110_key(&kb, 0x01, 0));
    CHECK_EQ(exchange(MC_M0110_INQUIRY, 3000), 0x83);
    /* Control is Command ($37: $6F); the right Shift is Shift ($71). */
    CHECK(mc_m0110_key(&kb, 0x36, 1));
    CHECK(mc_m0110_key(&kb, 0x7B, 1));
    CHECK_EQ(exchange(MC_M0110_INQUIRY, 3000), 0x6F);
    CHECK_EQ(exchange(MC_M0110_INQUIRY, 3000), 0x71);
    CHECK(mc_m0110_key(&kb, 0x7B, 0));
    CHECK_EQ(exchange(MC_M0110_INQUIRY, 3000), 0xF1);
    /* The up arrow: the prefix, then Instant brings $0D << 1 | 1 (the
     * ROM adds $40: $4D). The keypad's 5 ($57): $79, $2F. */
    CHECK(mc_m0110_key(&kb, 0x3E, 1));
    CHECK_EQ(exchange(MC_M0110_INQUIRY, 3000), MC_M0110_KEYPAD);
    CHECK_EQ(exchange(MC_M0110_INSTANT, 3000), 0x1B);
    CHECK(mc_m0110_key(&kb, 0x57, 0));
    CHECK_EQ(exchange(MC_M0110_INQUIRY, 3000), MC_M0110_KEYPAD);
    CHECK_EQ(exchange(MC_M0110_INSTANT, 3000), 0xAF);
    /* The keypad's * : Shift with the right arrow's code, Shift released. */
    CHECK(mc_m0110_key(&kb, 0x43, 1));
    CHECK_EQ(exchange(MC_M0110_INQUIRY, 3000), 0x71);
    CHECK_EQ(exchange(MC_M0110_INQUIRY, 3000), MC_M0110_KEYPAD);
    CHECK_EQ(exchange(MC_M0110_INSTANT, 3000), 0x05);
    CHECK_EQ(exchange(MC_M0110_INQUIRY, 3000), 0xF1);
    /* Model Number resets the keyboard: waiting transitions are lost. */
    CHECK(mc_m0110_key(&kb, 0x00, 1));
    CHECK_EQ(exchange(MC_M0110_MODEL, 3000), 0x0B);
    CHECK_EQ(exchange(MC_M0110_INSTANT, 3000), MC_M0110_NULL);
    /* A reply waits for mode 3. */
    CHECK(mc_m0110_key(&kb, 0x02, 1));
    via_write(MC_VIA_ACR, 0x1C);
    via_write(MC_VIA_SR, MC_M0110_INQUIRY);
    hw_fake_step(&fake, US(3000));
    via_write(MC_VIA_IFR, MC_VIA_IRQ_SR);
    hw_fake_step(&fake, US(10000));
    CHECK(!(via_read(MC_VIA_IFR) & MC_VIA_IRQ_SR));
    via_write(MC_VIA_ACR, 0x0C);
    via_read(MC_VIA_SR);
    hw_fake_step(&fake, US(1000));
    CHECK_EQ(via_read(MC_VIA_SR), 0x05);
    /* The M0110: model 1, no arrows or keypad. */
    keyboard(MC_M0110);
    CHECK_EQ(exchange(MC_M0110_MODEL, 3000), 0x03);
    CHECK(mc_m0110_key(&kb, 0x3E, 1));
    CHECK(mc_m0110_key(&kb, 0x57, 1));
    CHECK_EQ(exchange(MC_M0110_INSTANT, 3000), MC_M0110_NULL);
}

/* --- the mouse ------------------------------------------------------------------------ */

static mc_quadmouse mouse;
static int ready_x = 1;
static int ready(void *ctx, int axis) { (void)ctx; return axis ? 1 : ready_x; }

static int level(int line) { return hw_fake_line(&fake, &mouse, line); }

static void test_mouse(void) {
    hw_fake_init(&fake);
    mc_quadmouse_init(&mouse, &fake.host, "Mouse");
    mouse.ready = ready;
    mouse.ready_ctx = NULL;
    /* The button. */
    CHECK(mc_quadmouse_move(&mouse, 0, 0, 1));
    CHECK_EQ(level(MC_QUADMOUSE_LINE_BUTTON), 1);
    CHECK(mc_quadmouse_move(&mouse, 0, 0, 0));
    CHECK_EQ(level(MC_QUADMOUSE_LINE_BUTTON), 0);
    /* Right and up: each step toggles X1 (Y1) with X2 already different
     * from it (Y2 different: up). */
    CHECK(mc_quadmouse_move(&mouse, 2, -3, 0));
    CHECK_EQ(hw_fake_deadline(&fake, &mouse), MC_QUADMOUSE_STEP);
    for (int i = 0; i < 2; i++) {
        hw_fake_step(&fake, MC_QUADMOUSE_STEP);
        CHECK_EQ(hw_fake_changes(&fake, &mouse, MC_QUADMOUSE_LINE_X1), (unsigned)(i + 1));
        CHECK(level(MC_QUADMOUSE_LINE_X2) != level(MC_QUADMOUSE_LINE_X1));
        CHECK(level(MC_QUADMOUSE_LINE_Y2) != level(MC_QUADMOUSE_LINE_Y1));
    }
    hw_fake_step(&fake, MC_QUADMOUSE_STEP);
    CHECK_EQ(hw_fake_changes(&fake, &mouse, MC_QUADMOUSE_LINE_X1), 2u);
    CHECK_EQ(hw_fake_changes(&fake, &mouse, MC_QUADMOUSE_LINE_Y1), 3u);
    CHECK_EQ(hw_fake_deadline(&fake, &mouse), MC_NEVER);
    /* Left and down: X2 equal to X1, Y2 equal to Y1. */
    CHECK(mc_quadmouse_move(&mouse, -1, 1, 0));
    hw_fake_step(&fake, MC_QUADMOUSE_STEP);
    CHECK(level(MC_QUADMOUSE_LINE_X2) == level(MC_QUADMOUSE_LINE_X1));
    CHECK(level(MC_QUADMOUSE_LINE_Y2) == level(MC_QUADMOUSE_LINE_Y1));
    /* An axis not ready waits; its steps come when it is. */
    ready_x = 0;
    CHECK(mc_quadmouse_move(&mouse, 2, 0, 0));
    hw_fake_step(&fake, 5 * MC_QUADMOUSE_STEP);
    CHECK_EQ(hw_fake_changes(&fake, &mouse, MC_QUADMOUSE_LINE_X1), 3u);
    CHECK_EQ(mouse.dx, 2);
    ready_x = 1;
    hw_fake_step(&fake, 2 * MC_QUADMOUSE_STEP);
    CHECK_EQ(hw_fake_changes(&fake, &mouse, MC_QUADMOUSE_LINE_X1), 5u);
    CHECK_EQ(mouse.dx, 0);
}

/* --- video and sound ------------------------------------------------------------------ */

static void test_video(void) {
    enum { RAM = 128u << 10 };
    static uint8_t ram[RAM];
    static mc_compactvideo v;
    hw_fake_init(&fake);
    mc_compactvideo_init(&v, &fake.host, "Video", ram, RAM);
    mc_compactvideo_ops.reset(&v);
    /* The buffers at the top of the 128K's RAM: $1A700 and $12700. */
    mc_compactvideo_set_pa(&v, 0x48);
    CHECK(mc_compactvideo_pixels(&v) == ram + 0x1A700);
    mc_compactvideo_set_pa(&v, 0x08);
    CHECK(mc_compactvideo_pixels(&v) == ram + 0x12700);
    /* Vertical blanking from line 342 to the frame's end (370 lines of 704). */
    CHECK_EQ(hw_fake_deadline(&fake, &v), 342u * 704u);
    hw_fake_advance(&fake, 342u * 704u - 1u);
    CHECK_EQ(hw_fake_line(&fake, &v, MC_COMPACTVIDEO_LINE_VBL), 0);
    hw_fake_step(&fake, 1);
    CHECK_EQ(hw_fake_line(&fake, &v, MC_COMPACTVIDEO_LINE_VBL), 1);
    CHECK_EQ(hw_fake_deadline(&fake, &v), MC_COMPACTVIDEO_FRAME);
    hw_fake_advance(&fake, MC_COMPACTVIDEO_FRAME);
    CHECK_EQ(hw_fake_line(&fake, &v, MC_COMPACTVIDEO_LINE_VBL), 0);
    CHECK_EQ(hw_fake_changes(&fake, &v, MC_COMPACTVIDEO_LINE_VBL), 2u);
    /* 60.15 frames a second: 22,255 samples (C15M / 704). */
    CHECK_EQ(MC_COMPACTVIDEO_RATE, 22255u);
    int16_t out[2 * 1024];
    /* The first frame's samples (sound off: the rest level). */
    CHECK_EQ(mc_compactvideo_pull(&v, out, 1024), 370u);
    CHECK_EQ(out[0], 0);
    /* The main sound buffer ($1FD00, PA3 high): a square wave in the high
     * bytes (the low bytes, the disk speed, ignored), volume 7, PB7 low. */
    for (unsigned i = 0; i < 370; i++) {
        ram[0x1FD00 + 2 * i] = (i / 37) & 1 ? 0xE0 : 0x20;
        ram[0x1FD00 + 2 * i + 1] = 0x55;
    }
    mc_compactvideo_set_pa(&v, 0x4F);
    mc_compactvideo_set_pb7(&v, 0);
    hw_fake_advance(&fake, MC_COMPACTVIDEO_FRAME + 342u * 704u);
    CHECK_EQ(mc_compactvideo_pull(&v, out, 1024), 370u);
    int highs = 0, lows = 0;
    for (unsigned i = 0; i < 370; i++) {
        highs += out[2 * i] > 4000;
        lows += out[2 * i] < -4000;
        CHECK_EQ(out[2 * i], out[2 * i + 1]);
    }
    CHECK(highs > 100 && lows > 100 && v.sounding);
    /* The alternate buffer ($1A100) holds silence; PA3 low selects it. */
    mc_compactvideo_set_pa(&v, 0x47);
    for (unsigned i = 0; i < 370; i++) ram[0x1A100 + 2 * i] = 0x80;
    hw_fake_advance(&fake, 2 * MC_COMPACTVIDEO_FRAME + 342u * 704u);
    hw_fake_advance(&fake, 3 * MC_COMPACTVIDEO_FRAME + 342u * 704u);
    CHECK_EQ(mc_compactvideo_pull(&v, out, 1024), 740u);
    int loud = 0;
    for (unsigned i = 370; i < 740; i++) loud += out[2 * i] > 300 || out[2 * i] < -300;
    CHECK_EQ(loud, 0);
    /* PB7 high in the middle of a frame silences the lines after it. */
    mc_compactvideo_set_pa(&v, 0x4F);
    for (unsigned i = 0; i < 370; i++) ram[0x1FD00 + 2 * i] = (i & 1) ? 0xFF : 0x00;
    hw_fake_advance(&fake, 4 * MC_COMPACTVIDEO_FRAME + 100u * 704u);
    mc_compactvideo_set_pb7(&v, 1);
    hw_fake_advance(&fake, 4 * MC_COMPACTVIDEO_FRAME + 342u * 704u);
    CHECK_EQ(mc_compactvideo_pull(&v, out, 1024), 370u);
    int jumps = 0;      /* after PB7: no more steps, only the filter's decay */
    for (unsigned i = 200; i < 370; i++) jumps += abs(out[2 * i] - out[2 * i - 2]) > 300;
    CHECK_EQ(jumps, 0);
    loud = 0;
    for (unsigned i = 10; i < 100; i++) loud += out[2 * i] > 4000 || out[2 * i] < -4000;
    CHECK(loud > 80);
    /* Horizontal blanking (VIA PB6): the 192 dots after each line's 512
     * visible ones, from line 0 of the frame, in the vertical blanking too. */
    const uint64_t f0 = v.frame;
    CHECK(!mc_compactvideo_hblank(&v, f0) && !mc_compactvideo_hblank(&v, f0 + 511));
    CHECK(mc_compactvideo_hblank(&v, f0 + 512) && mc_compactvideo_hblank(&v, f0 + 703));
    CHECK(!mc_compactvideo_hblank(&v, f0 + 704) && mc_compactvideo_hblank(&v, f0 + 100 * 704 + 600));
    CHECK(mc_compactvideo_hblank(&v, f0 + 360 * 704 + 520) && !mc_compactvideo_hblank(&v, f0 + 360 * 704 + 10));
    unsigned blank = 0;
    for (uint64_t t = 0; t < MC_COMPACTVIDEO_FRAME; t++) blank += (unsigned)mc_compactvideo_hblank(&v, f0 + t);
    CHECK_EQ(blank, 192u * 370u);
    /* The disk-speed bytes: places in the PWM counter's sequence from $1E
     * ($1E, $2F, $17, $0B, ...), all 63 states once; the buffer's mean
     * place sets the motor: 650 rpm at place 0, 300 at 44. */
    CHECK(mc_compactvideo_pwm_place(0x1E) == 0 && mc_compactvideo_pwm_place(0x2F) == 1 && mc_compactvideo_pwm_place(0x17) == 2);
    CHECK(mc_compactvideo_pwm_place(0x0B) == 3 && mc_compactvideo_pwm_place(0xCB) == 3 && mc_compactvideo_pwm_place(0) == 62);
    unsigned seen = 0;
    for (unsigned b = 1; b < 64; b++) seen |= mc_compactvideo_pwm_place((uint8_t)b) < 63 ? 0 : 1u;
    CHECK_EQ(seen, 0u);
    for (unsigned i = 0; i < 370; i++) ram[0x1FD00 + 2 * i + 1] = 0x1E;
    CHECK_EQ(mc_compactvideo_speed(&v), 650u * 16u);
    uint8_t x = 0x1E;                   /* place 22: halfway, 475 rpm */
    for (int k = 0; k < 22; k++) x = (uint8_t)((x >> 1) | (((x ^ (x >> 1)) & 1u) << 5));
    for (unsigned i = 0; i < 370; i++) ram[0x1FD00 + 2 * i + 1] = x;
    CHECK_EQ(mc_compactvideo_speed(&v), 475u * 16u);
    /* PA3 low: the alternate buffer's speed bytes. */
    mc_compactvideo_set_pa(&v, 0x47);
    for (unsigned i = 0; i < 370; i++) ram[0x1A100 + 2 * i + 1] = 0x1E;
    CHECK_EQ(mc_compactvideo_speed(&v), 650u * 16u);
}

/* --- the boards ------------------------------------------------------------------------ */

static mc_machine *machine(const char *model, uint32_t checksum, uint32_t rom_size, uint32_t ram_kb, uint8_t **rom_out) {
    uint8_t *rom = calloc(1, rom_size);
    if (!rom) return NULL;
    for (int i = 0; i < 4; i++) rom[i] = (uint8_t)(checksum >> (24 - 8 * i));
    rom[rom_size / 2] = 0xA5;          /* tells a repeat from the ROM's second half */
    mc_config c;
    mc_config_defaults(&c);
    c.model = (uint32_t)mc_model_index(mc_model_find(model));
    mc_config_set_ram_kb(&c, ram_kb);
    c.width = 512; c.height = 342; c.depth = 1;
    c.disk_count = 0;
    c.nvram[0] = 0;
    char error[256] = "";
    mc_machine *m = mc_machine_create(&c, rom, rom_size, error, sizeof error);
    if (!m) printf("%s: %s\n", model, error);
    *rom_out = rom;
    return m;
}

static uint32_t rd(mc_bus *b, uint32_t a, unsigned size) {
    uint32_t v = 0;
    CHECK_EQ(mc_bus_read(b, a, size, &v), 0);   /* never a bus error */
    return v;
}
static void wr(mc_bus *b, uint32_t a, unsigned size, uint32_t v) { CHECK_EQ(mc_bus_write(b, a, size, v), 0); }

#define VIA(reg) (0xEFE1FEu + (reg) * 0x200u)

static void test_boards(void) {
    uint8_t *rom = NULL;
    mc_machine *m = machine("mac-plus", 0x4D1F8172u, 128u << 10, 4096, &rom);
    CHECK(m != NULL);
    if (!m) { free(rom); return; }
    mc_hw *hw = mc_machine_hw(m);
    mc_bus *bus = &hw->bus;
    CHECK(hw->ram_size == 4u << 20);
    /* The overlay: the ROM at 0, the RAM at $60 0000. */
    CHECK_EQ(rd(bus, 0, 4), 0x4D1F8172u);
    wr(bus, 0x600100u, 4, 0x12345678u);
    CHECK_EQ(hw->ram[0x100], 0x12);
    /* PA4 low: the RAM at 0, through the 4 MB space. */
    wr(bus, VIA(MC_VIA_DDRA), 1, 0x7F);
    wr(bus, VIA(MC_VIA_ORA_NH), 1, 0x68);
    CHECK_EQ(rd(bus, 0x100, 4), 0x12345678u);
    CHECK_EQ(rd(bus, 0x600100u, 4), 0);
    /* The ROM: $40 0000 and its repeat at $42 0000; nothing at $44 0000. */
    CHECK_EQ(rd(bus, 0x400000u, 4), 0x4D1F8172u);
    CHECK_EQ(rd(bus, 0x420000u, 4), 0x4D1F8172u);
    CHECK_EQ(rd(bus, 0x410000u, 1), 0xA5);
    CHECK(rd(bus, 0x440000u, 4) != 0x4D1F8172u);
    /* Nothing at $F0 0000 or $F8 0000, and no bus error. */
    CHECK_EQ(rd(bus, 0xF80000u, 4), 0);
    rd(bus, 0xF00000u, 2);
    /* The VIA on the high byte: IER at $EF FDFE; the low byte is nothing. */
    wr(bus, VIA(MC_VIA_IER), 1, 0x84);
    CHECK_EQ(rd(bus, VIA(MC_VIA_IER), 1), 0x84);
    CHECK_EQ(rd(bus, VIA(MC_VIA_IER) + 1, 1), 0xFF);
    /* The SCC: WR12 of channel B written at $BF FFF9, read back as RR12 at
     * $9F FFF8; a write to the read space does nothing. */
    wr(bus, 0xBFFFF9u, 1, 12);
    wr(bus, 0xBFFFF9u, 1, 0x5A);
    wr(bus, 0xBFFFF9u, 1, 12);
    CHECK_EQ(rd(bus, 0x9FFFF8u, 1), 0x5A);
    wr(bus, 0x9FFFF8u, 1, 12);
    wr(bus, 0xBFFFF9u, 1, 12);
    CHECK_EQ(rd(bus, 0x9FFFF8u, 1), 0x5A);
    CHECK_EQ(rd(bus, 0xBFFFF9u, 1), 0xFF);
    free(rom);
    mc_machine_destroy(m);
    /* The 128K: 128 KB repeating through the 4 MB space; its ROM through
     * $40 0000-$4F FFFF. */
    m = machine("mac-128k", 0x28BA61CEu, 64u << 10, 1024, &rom);
    CHECK(m != NULL);
    if (!m) { free(rom); return; }
    hw = mc_machine_hw(m);
    bus = &hw->bus;
    CHECK(hw->ram_size == 128u << 10);
    wr(bus, VIA(MC_VIA_DDRA), 1, 0x7F);
    wr(bus, VIA(MC_VIA_ORA_NH), 1, 0x68);
    wr(bus, 0x10, 4, 0xCAFEF00Du);
    CHECK_EQ(rd(bus, 0x20010u, 4), 0xCAFEF00Du);
    CHECK_EQ(rd(bus, 0x3E0010u, 4), 0xCAFEF00Du);
    CHECK_EQ(rd(bus, 0x4F0000u, 4), 0x28BA61CEu);
    /* No 5380: its addresses read nothing. */
    rd(bus, 0x580000u, 1);
    free(rom);
    mc_machine_destroy(m);
    /* The Plus with 2.5 MB: the 1 MB SIMMs' 2 MB where A21 is low, the
     * 256 KB SIMMs' 512 KB repeated where it is high; the screen at
     * $3F A700 is in the second row. */
    m = machine("mac-plus", 0x4D1F8172u, 128u << 10, 2560, &rom);
    CHECK(m != NULL);
    if (!m) { free(rom); return; }
    hw = mc_machine_hw(m);
    bus = &hw->bus;
    CHECK(hw->ram_size == 2560u << 10);
    wr(bus, VIA(MC_VIA_DDRA), 1, 0x7F);
    wr(bus, VIA(MC_VIA_ORA_NH), 1, 0x68);
    wr(bus, 0x1FFFFCu, 4, 0x11223344u);
    wr(bus, 0x200000u, 4, 0x55667788u);
    CHECK_EQ(hw->ram[0x1FFFFC], 0x11);
    CHECK_EQ(hw->ram[0x200000], 0x55);
    CHECK_EQ(rd(bus, 0x280000u, 4), 0x55667788u);
    CHECK_EQ(rd(bus, 0x380000u, 4), 0x55667788u);
    CHECK_EQ(rd(bus, 0x000000u, 4) == 0x55667788u, 0);
    wr(bus, 0x3FA700u, 1, 0xA5);
    CHECK_EQ(hw->ram[0x27A700], 0xA5);
    mc_framebuffer fb;
    CHECK(mc_machine_framebuffer(m, &fb) && fb.pixels == hw->ram + 0x27A700);
    /* VIA PB6: the horizontal blanking as the video's line stands. The
     * processor spins on BRA.S * in RAM (the test ROM holds no code), so
     * guest time advances by the same steps on every build. */
    wr(bus, 0x0, 4, 0x00010000u);           /* SSP */
    wr(bus, 0x4, 4, 0x00002000u);           /* PC */
    wr(bus, 0x2000, 2, 0x60FEu);            /* BRA.S * */
    m68k_reset(hw->cpu);
    unsigned high = 0;
    for (int i = 0; i < 64; i++) {
        mc_machine_run(m, 20, 0);
        high += (rd(bus, VIA(MC_VIA_ORB), 1) >> 6) & 1u;
    }
    CHECK(high > 0 && high < 64);
    free(rom);
    mc_machine_destroy(m);
}

int main(void) {
    test_keyboard();
    test_mouse();
    test_video();
    test_boards();
    printf("test_compact: %d failure(s)\n", mc_test_failures);
    return mc_test_failures != 0;
}
