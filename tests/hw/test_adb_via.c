/* The ADB devices and the VIA transceiver against the Guide to the Macintosh
 * Family Hardware, 2nd ed., chapter 8: Talk/Listen/SendReset/Flush,
 * register 3 and the reserved handler IDs, collisions and address moves,
 * Service Requests, the Extended Keyboard and Standard Mouse registers; the
 * transaction states (Table 8-12) driven the way the Quadra 700 ROM drives
 * them, with /ADB.INT as its driver reads it (adb_via.h). */
#include "hw_test.h"
#include "../../src/hw/adb.h"
#include "../../src/hw/adb_via.h"
#include "../../src/hw/via.h"

/* --- the devices --------------------------------------------------------------------- */

static void test_devices(void) {
    mc_adb_bus bus;
    mc_adb_keyboard kbd;
    mc_adb_mouse mouse, mouse2;
    mc_adb_bus_init(&bus);
    mc_adb_keyboard_init(&kbd);
    mc_adb_mouse_init(&mouse);
    mc_adb_attach(&bus, &kbd.dev);
    mc_adb_attach(&bus, &mouse.dev);
    uint8_t out[8];
    /* Talk register 3: exceptional event, SRQ enable, address, handler. */
    CHECK_EQ(mc_adb_command(&bus, 0x2F, NULL, 0, out), 2);
    CHECK_EQ(out[0], 0x62);
    CHECK_EQ(out[1], 0x02);
    CHECK_EQ(mc_adb_command(&bus, 0x3F, NULL, 0, out), 2);
    CHECK_EQ(out[1], 0x01);
    CHECK_EQ(mc_adb_command(&bus, 0x5F, NULL, 0, out), 0);         /* nobody at 5 */
    /* Nothing to say: no answer to register 0. */
    CHECK_EQ(mc_adb_command(&bus, 0x2C, NULL, 0, out), 0);
    CHECK_EQ(mc_adb_command(&bus, 0x3C, NULL, 0, out), 0);
    /* Two transitions per Talk, $FF when only one is left. */
    CHECK(mc_adb_keyboard_key(&kbd, 0x00, 1));
    CHECK(mc_adb_keyboard_key(&kbd, 0x00, 0));
    CHECK(mc_adb_keyboard_key(&kbd, 0x31, 1));
    CHECK(mc_adb_srq(&bus, 3));                                    /* the keyboard requests service */
    CHECK(!mc_adb_srq(&bus, 2));
    CHECK_EQ(mc_adb_command(&bus, 0x2C, NULL, 0, out), 2);
    CHECK_EQ(out[0], 0x00);
    CHECK_EQ(out[1], 0x80);
    CHECK_EQ(mc_adb_command(&bus, 0x2C, NULL, 0, out), 2);
    CHECK_EQ(out[0], 0x31);
    CHECK_EQ(out[1], 0xFF);
    /* Register 2: Command held reads 0 in bit 8; the LEDs from Listen 2. */
    mc_adb_keyboard_key(&kbd, 0x37, 1);
    mc_adb_command(&bus, 0x2C, NULL, 0, out);
    CHECK_EQ(mc_adb_command(&bus, 0x2E, NULL, 0, out), 2);
    CHECK_EQ(out[0], 0xFE);
    const uint8_t leds[2] = {0xFF, 0xFA};
    mc_adb_command(&bus, 0x2A, leds, 2, NULL);
    CHECK_EQ(kbd.leds, 2);
    /* Handler 2 sends the left codes for the right-hand modifiers; 3 does not. */
    mc_adb_keyboard_key(&kbd, 0x7B, 1);
    mc_adb_command(&bus, 0x2C, NULL, 0, out);
    CHECK_EQ(out[0], 0x38);
    const uint8_t h3[2] = {0x62, 0x03}, h7[2] = {0x62, 0x07};
    mc_adb_command(&bus, 0x2B, h7, 2, NULL);                      /* unknown handler: ignored */
    CHECK_EQ(kbd.dev.handler, 2);
    mc_adb_command(&bus, 0x2B, h3, 2, NULL);
    CHECK_EQ(kbd.dev.handler, 3);
    mc_adb_keyboard_key(&kbd, 0x7B, 0);
    mc_adb_command(&bus, 0x2C, NULL, 0, out);
    CHECK_EQ(out[0], 0xFB);
    /* The mouse: Y in bits 14-8, X in 6-0, at most 63 a report, the button
     * in bit 15 (0 = down). */
    mc_adb_mouse_move(&mouse, 100, -5, 1);
    CHECK_EQ(mc_adb_command(&bus, 0x3C, NULL, 0, out), 2);
    CHECK_EQ(out[0], 0x7B);
    CHECK_EQ(out[1], 0xBF);
    CHECK_EQ(mc_adb_command(&bus, 0x3C, NULL, 0, out), 2);
    CHECK_EQ(out[0], 0x00);
    CHECK_EQ(out[1], 0x80 | 37);
    CHECK_EQ(mc_adb_command(&bus, 0x3C, NULL, 0, out), 0);
    mc_adb_mouse_move(&mouse, 0, 0, 0);
    CHECK_EQ(mc_adb_command(&bus, 0x3C, NULL, 0, out), 2);
    CHECK_EQ(out[0], 0x80);
    /* Collisions: two mice at 3. The first answers; Listen 3 with $FE moves
     * the one that saw no collision. */
    mc_adb_mouse_init(&mouse2);
    mc_adb_attach(&bus, &mouse2.dev);
    CHECK_EQ(mc_adb_command(&bus, 0x3F, NULL, 0, out), 2);
    CHECK(!mouse.dev.collided);
    CHECK(mouse2.dev.collided);
    const uint8_t move[2] = {0x09, 0xFE};
    mc_adb_command(&bus, 0x3B, move, 2, NULL);
    CHECK_EQ(mouse.dev.address, 9);
    CHECK_EQ(mouse2.dev.address, 3);
    CHECK_EQ(mc_adb_command(&bus, 0x9F, NULL, 0, out), 2);
    CHECK_EQ(out[0] & 0x0F, 9);
    /* $00: address and SRQ enable. */
    const uint8_t set[2] = {0x0C, 0x00};
    mc_adb_command(&bus, 0x9B, set, 2, NULL);
    CHECK_EQ(mouse.dev.address, 12);
    CHECK_EQ(mouse.dev.srq_enable, 0);
    /* SendReset: every device back to its defaults. */
    mc_adb_command(&bus, 0x00, NULL, 0, NULL);
    CHECK_EQ(mouse.dev.address, 3);
    CHECK_EQ(kbd.dev.handler, 2);
    CHECK_EQ(mouse.dev.srq_enable, 1);
}

/* --- the transceiver behind a VIA ----------------------------------------------------- */

static hw_fake h;     /* one clock for the VIA and the transceiver */
static mc_via via;
static mc_adb_via xcvr;
static mc_adb_bus bus;
static mc_adb_keyboard kbd;
static mc_adb_mouse mouse;

static uint8_t shift(void *ctx, uint8_t in) { (void)ctx; return mc_via_shift8(&via, in); }
static void wire(hw_fake *f, void *dev, int line, int state) {
    (void)f;
    if (dev == &via && line == MC_VIA_LINE_PB) mc_adb_via_state(&xcvr, ((unsigned)state >> 4) & 3u);
    if (dev == &xcvr && line == MC_ADB_VIA_LINE_INT) mc_via_set_pb(&via, state ? 0x08u : 0, 0x08u);
}

static uint8_t rd(unsigned reg) { return (uint8_t)mc_via_ops.read(&via, reg << 9, 1); }
static void wr(unsigned reg, uint8_t v) { mc_via_ops.write(&via, reg << 9, 1, v); }
static void set_state(unsigned st) { wr(MC_VIA_ORB, (uint8_t)((rd(MC_VIA_ORB) & 0xCFu) | st << 4)); }
static int int_low(void) { return !(rd(MC_VIA_ORB) & 0x08u); }

/* Lets the transceiver act; returns whether a shift-register interrupt came.
 * (The VIA arms no deadline in the externally clocked shift modes, so the
 * host's one deadline is the transceiver's.) */
static int run(uint64_t time) {
    hw_fake_advance(&h, h.now + time);
    return (rd(MC_VIA_IFR) & MC_VIA_IRQ_SR) != 0;
}

static void start_command(uint8_t cmd) {
    wr(MC_VIA_ACR, 0x1C);           /* shift out under CB1 */
    wr(MC_VIA_SR, cmd);
    set_state(0);
}
/* The next byte in: the host reads SR (input mode) and toggles the state. */
static void next_byte(unsigned st) {
    wr(MC_VIA_ACR, 0x0C);           /* shift in under CB1 */
    rd(MC_VIA_SR);
    set_state(st);
}

static void setup(void) {
    hw_fake_init(&h);
    h.on_line = wire;
    mc_adb_bus_init(&bus);
    mc_adb_keyboard_init(&kbd);
    mc_adb_mouse_init(&mouse);
    mc_adb_attach(&bus, &kbd.dev);
    mc_adb_attach(&bus, &mouse.dev);
    mc_via_init(&via, &h.host, "VIA1", 9);
    mc_adb_via_init(&xcvr, &h.host, "ADB", &bus, shift, NULL);
    mc_via_ops.reset(&via);
    mc_adb_via_ops.reset(&xcvr);
    wr(MC_VIA_DDRB, 0x30);
    wr(MC_VIA_ORB, 0x30);           /* state 3: idle */
}

static void test_transceiver(void) {
    setup();
    CHECK_EQ(xcvr.st, 3);
    CHECK(!int_low());
    /* Talk register 3 to the keyboard: the command goes out in state 0. */
    start_command(0x2F);
    CHECK(!run(MC_ADB_COMMAND_TIME - 1));
    CHECK(run(2));
    CHECK(!int_low());
    /* Even, odd, even: two bytes, then a third with /INT low (the end). */
    next_byte(1);
    CHECK(run(2 * MC_ADB_BYTE_TIME));
    CHECK(!int_low());
    const uint8_t b1 = rd(MC_VIA_SR);
    set_state(2);
    CHECK(run(MC_ADB_BYTE_TIME));
    CHECK(!int_low());                                              /* no Service Request */
    const uint8_t b2 = rd(MC_VIA_SR);
    set_state(1);
    CHECK(run(MC_ADB_BYTE_TIME));
    CHECK(int_low());                                               /* nothing more */
    CHECK_EQ(b1, 0x62);
    CHECK_EQ(b2, 0x02);
    /* A Talk to an empty address: the first byte comes with /INT low. */
    set_state(0);
    start_command(0x5F);
    CHECK(run(MC_ADB_COMMAND_TIME));
    next_byte(1);
    CHECK(run(2 * MC_ADB_BYTE_TIME));
    CHECK(int_low());
    /* Listen register 2 to the keyboard: two bytes out in states 1 and 2. */
    start_command(0x2A);
    CHECK(run(MC_ADB_COMMAND_TIME));
    wr(MC_VIA_SR, 0xFF);
    set_state(1);
    CHECK(run(MC_ADB_BYTE_TIME));
    wr(MC_VIA_SR, 0xFC);
    set_state(2);
    CHECK(run(MC_ADB_BYTE_TIME));
    set_state(3);
    CHECK_EQ(kbd.leds, 4);
    /* A Service Request reaches the host with the second byte: the mouse
     * answers while the keyboard has a key. */
    mc_adb_mouse_move(&mouse, 3, 4, 0);
    mc_adb_keyboard_key(&kbd, 0x0C, 1);
    start_command(0x3C);
    CHECK(run(MC_ADB_COMMAND_TIME));
    next_byte(1);
    CHECK(run(2 * MC_ADB_BYTE_TIME));
    CHECK(!int_low());
    CHECK_EQ(rd(MC_VIA_SR), 0x84);
    set_state(2);
    CHECK(run(MC_ADB_BYTE_TIME));
    CHECK(int_low());                                               /* the keyboard's request */
    CHECK_EQ(rd(MC_VIA_SR), 0x83);
    set_state(1);
    CHECK(run(MC_ADB_BYTE_TIME));
    CHECK(int_low());
    /* Idle in state 3: polling repeats the last Talk (the mouse at 3) every
     * 11 ms; nothing happens while it has nothing to say. */
    mc_adb_command(&bus, 0x2C, NULL, 0, (uint8_t[8]){0});           /* the key is read elsewhere */
    rd(MC_VIA_SR);
    set_state(3);
    CHECK(!run(3 * MC_ADB_POLL_TIME));
    /* The mouse moves: polling finds it and shifts a byte in to say so;
     * the host reads the answer in states 1 and 2. */
    mc_adb_mouse_move(&mouse, -1, 0, 0);
    mc_adb_via_poke(&xcvr);
    CHECK(run(MC_ADB_POLL_TIME + 1));
    next_byte(1);
    CHECK(run(2 * MC_ADB_BYTE_TIME));
    CHECK(!int_low());
    CHECK_EQ(rd(MC_VIA_SR), 0x80);
    set_state(2);
    CHECK(run(MC_ADB_BYTE_TIME));
    CHECK_EQ(rd(MC_VIA_SR), 0xFF);                                  /* X = -1 */
    /* A command while polling holds an answer: /INT low as the command
     * goes out, and the held answer is read instead. */
    set_state(3);
    mc_adb_mouse_move(&mouse, 2, 0, 0);
    CHECK(run(MC_ADB_POLL_TIME + 1));
    start_command(0x2F);
    CHECK(run(MC_ADB_COMMAND_TIME));
    CHECK(int_low());
    next_byte(1);
    CHECK(run(2 * MC_ADB_BYTE_TIME));
    CHECK_EQ(rd(MC_VIA_SR), 0x80);
    set_state(2);
    CHECK(run(MC_ADB_BYTE_TIME));
    CHECK_EQ(rd(MC_VIA_SR), 0x82);
}

/* The Applejack (adb.h): a mouse until its handler is $46, then the four
 * bytes of the technical note's Figure 6. */
static void test_applejack(void) {
    mc_adb_bus line;
    mc_adb_applejack pad;
    uint8_t out[8];
    mc_adb_bus_init(&line);
    mc_adb_applejack_init(&pad);
    mc_adb_attach(&line, &pad.dev);
    CHECK_EQ(mc_adb_command(&line, 0x3F, NULL, 0, out), 2);
    CHECK_EQ(out[0] & 0x0F, 3);
    CHECK_EQ(out[1], 0x01);
    CHECK_EQ(mc_adb_command(&line, 0x3C, NULL, 0, out), 0);
    /* Mouse mode: Left Fire is the button, Right Fire the second button's
     * bit; the trackball's counts as a mouse's. */
    mc_adb_applejack_move(&pad, 5, -3);
    mc_adb_applejack_switch(&pad, MC_APPLEJACK_LEFT_FIRE, 1);
    CHECK_EQ(mc_adb_command(&line, 0x3C, NULL, 0, out), 2);
    CHECK_EQ(out[0], 0x7D);
    CHECK_EQ(out[1], 0x85);
    CHECK_EQ(mc_adb_command(&line, 0x3C, NULL, 0, out), 0);
    mc_adb_applejack_switch(&pad, MC_APPLEJACK_RIGHT_FIRE, 1);
    mc_adb_applejack_switch(&pad, MC_APPLEJACK_LEFT_FIRE, 0);
    CHECK_EQ(mc_adb_command(&line, 0x3C, NULL, 0, out), 2);
    CHECK_EQ(out[0], 0x80);
    CHECK_EQ(out[1], 0x00);
    mc_adb_applejack_switch(&pad, MC_APPLEJACK_RIGHT_FIRE, 0);
    CHECK_EQ(mc_adb_command(&line, 0x3C, NULL, 0, out), 2);
    /* Another switch makes it answer, with nothing changed in the two bytes. */
    mc_adb_applejack_switch(&pad, MC_APPLEJACK_GREEN, 1);
    CHECK_EQ(mc_adb_command(&line, 0x3C, NULL, 0, out), 2);
    CHECK_EQ(out[0], 0x80);
    CHECK_EQ(out[1], 0x80);
    mc_adb_applejack_switch(&pad, MC_APPLEJACK_GREEN, 0);
    CHECK_EQ(mc_adb_command(&line, 0x3C, NULL, 0, out), 2);
    /* A handler it does not know is refused; $46 is taken. */
    const uint8_t extended[2] = {0x03, 0x04}, controller[2] = {0x03, 0x46};
    mc_adb_command(&line, 0x3B, extended, 2, out);
    CHECK_EQ(mc_adb_command(&line, 0x3F, NULL, 0, out), 2);
    CHECK_EQ(out[1], 0x01);
    mc_adb_command(&line, 0x3B, controller, 2, out);
    CHECK_EQ(mc_adb_command(&line, 0x3F, NULL, 0, out), 2);
    CHECK_EQ(out[1], 0x46);
    CHECK_EQ(mc_adb_command(&line, 0x3C, NULL, 0, out), 0);
    /* Controller mode: four bytes, a switch down reads 0, b7-b3 read 1. */
    mc_adb_applejack_switch(&pad, MC_APPLEJACK_UP | MC_APPLEJACK_RED | MC_APPLEJACK_SQUARE, 1);
    mc_adb_applejack_move(&pad, -2, 1);
    CHECK_EQ(mc_adb_command(&line, 0x3C, NULL, 0, out), 4);
    CHECK_EQ(out[0], 0x81);
    CHECK_EQ(out[1], 0xFE);
    CHECK_EQ(out[2], 0xBB);
    CHECK_EQ(out[3], 0xFE);
    mc_adb_applejack_switch(&pad, MC_APPLEJACK_SWITCHES, 1);
    CHECK_EQ(mc_adb_command(&line, 0x3C, NULL, 0, out), 4);
    CHECK_EQ(out[0], 0x00);
    CHECK_EQ(out[1], 0x00);
    CHECK_EQ(out[2], 0x00);
    CHECK_EQ(out[3], 0xF8);
    mc_adb_applejack_switch(&pad, MC_APPLEJACK_SWITCHES, 0);
    CHECK_EQ(mc_adb_command(&line, 0x3C, NULL, 0, out), 4);
    CHECK_EQ(out[2], 0xFF);
    CHECK_EQ(out[3], 0xFF);
    CHECK_EQ(mc_adb_command(&line, 0x3C, NULL, 0, out), 0);
    /* The bus reset returns it to the mouse. */
    mc_adb_reset(&line);
    CHECK_EQ(mc_adb_command(&line, 0x3F, NULL, 0, out), 2);
    CHECK_EQ(out[1], 0x01);
}

int mc_test_adb(void) {
    test_applejack();
    test_devices();
    test_transceiver();
    return 0;
}
