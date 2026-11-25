/* The real-time clock chip against Inside Macintosh Volume III, "The
 * Real-Time Clock" (the serial protocol and its command table) and the
 * Guide to the Macintosh Family Hardware, 2nd ed. (256 bytes of parameter
 * RAM, the one-second interrupt); the extended command as the Quadra 700 ROM
 * uses it. */
#include "hw_test.h"
#include "../../src/hw/rtc.h"

static hw_fake f;
static mc_rtc r;

static void send(uint8_t byte) {
    for (int i = 7; i >= 0; i--) {
        const int bit = (byte >> i) & 1;
        mc_rtc_pins(&r, 0, 0, bit);
        mc_rtc_pins(&r, 0, 1, bit);
    }
}

static uint8_t receive(void) {
    uint8_t v = 0;
    for (int i = 0; i < 8; i++) {
        mc_rtc_pins(&r, 0, 0, 1);
        v = (uint8_t)(v << 1 | (mc_rtc_data(&r) & 1));
        mc_rtc_pins(&r, 0, 1, 1);
    }
    return v;
}

static void end(void) { mc_rtc_pins(&r, 1, 1, 1); }

static void write_cmd(uint8_t cmd, uint8_t data) {
    send(cmd);
    send(data);
    end();
}
static uint8_t read_cmd(uint8_t cmd) {
    send(cmd);
    const uint8_t v = receive();
    end();
    return v;
}
static void write_ext(uint8_t address, uint8_t data) {
    send((uint8_t)(0x38u | address >> 5));
    send((uint8_t)((address & 0x1Fu) << 2));
    send(data);
    end();
}
static uint8_t read_ext(uint8_t address) {
    send((uint8_t)(0xB8u | address >> 5));
    send((uint8_t)((address & 0x1Fu) << 2));
    const uint8_t v = receive();
    end();
    return v;
}

int mc_test_rtc(void) {
    hw_fake_init(&f);
    mc_rtc_init(&r, &f.host, "RTC");
    mc_rtc_ops.reset(&r);
    /* Write-protected at first: writes are ignored until bit 7 is cleared. */
    write_cmd(0x4D, 0x5A);                  /* z1aaaa01, a = 3 */
    CHECK_EQ(read_cmd(0xCD), 0x00);
    write_cmd(0x35, 0x00);                  /* write-protect register */
    write_cmd(0x4D, 0x5A);
    CHECK_EQ(read_cmd(0xCD), 0x5A);
    CHECK_EQ(r.pram[0x13], 0x5A);           /* original byte 3 is extended byte $13 */
    write_cmd(0x29, 0xC3);                  /* z010aa01, a = 2 */
    CHECK_EQ(read_cmd(0xA9), 0xC3);
    CHECK_EQ(r.pram[0x0A], 0xC3);
    /* The extended command reaches all 256 bytes. */
    write_ext(0xAB, 0x77);
    write_ext(0x00, 0x11);
    write_ext(0xFF, 0x22);
    CHECK_EQ(r.pram[0xAB], 0x77);
    CHECK_EQ(read_ext(0xAB), 0x77);
    CHECK_EQ(read_ext(0x00), 0x11);
    CHECK_EQ(read_ext(0xFF), 0x22);
    CHECK_EQ(read_ext(0x13), 0x5A);
    /* The seconds registers, written low to high, then counting. */
    write_cmd(0x01, 0x78);
    write_cmd(0x05, 0x56);
    write_cmd(0x09, 0x34);
    write_cmd(0x0D, 0x12);
    CHECK_EQ(mc_rtc_seconds(&r), 0x12345678);
    hw_fake_advance(&f, 3ull * MC_C15M_HZ + 5);
    CHECK_EQ(read_cmd(0x81), 0x7B);
    CHECK_EQ(read_cmd(0x85), 0x56);
    CHECK_EQ(read_cmd(0x8D), 0x12);
    /* The second form of the seconds commands (bit 4 set), which the ROM
     * reads and compares with the first. */
    CHECK_EQ(read_cmd(0x9D), 0x12);
    CHECK_EQ(read_cmd(0x95), 0x56);
    CHECK_EQ(read_cmd(0x91), 0x7B);
    /* The one-second signal: low at each second, high half a second later. */
    const unsigned edges = hw_fake_reports(&f, &r, MC_RTC_LINE_SECOND);
    CHECK(edges >= 5);
    hw_fake_advance(&f, 4ull * MC_C15M_HZ);
    CHECK_EQ(hw_fake_value(&f, &r, MC_RTC_LINE_SECOND), 0);
    hw_fake_advance(&f, 4ull * MC_C15M_HZ + MC_C15M_HZ / 2u);
    CHECK_EQ(hw_fake_value(&f, &r, MC_RTC_LINE_SECOND), 1);
    /* Raising enable in the middle of a byte aborts it. */
    send(0x38);
    mc_rtc_pins(&r, 0, 0, 1);
    mc_rtc_pins(&r, 0, 1, 1);
    end();
    write_cmd(0x4D, 0x99);
    CHECK_EQ(read_cmd(0xCD), 0x99);
    /* Write-protect again. */
    write_cmd(0x35, 0x80);
    write_cmd(0x4D, 0x00);
    write_ext(0xAB, 0x00);
    CHECK_EQ(read_cmd(0xCD), 0x99);
    CHECK_EQ(read_ext(0xAB), 0x77);
    /* The data line is released between transactions. */
    CHECK_EQ(mc_rtc_data(&r), 1);
    return 0;
}
