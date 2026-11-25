/* The 6522 VIA against the Rockwell R6522 data sheet: ports and data
 * direction, IFR/IER, Timer 1 one-shot and free-running with PB7 (Figures
 * 15-16: flag N + 1.5 cycles after the load, period N + 2), Timer 2
 * one-shot and pulse counting (Figures 18-19), the shift register modes
 * (Figures 22-28) and the CA/CB handshakes (Figures 8-11). One E cycle is
 * 20 C15M periods (Guide to the Macintosh Family Hardware, 2nd ed.,
 * chapter 4: 783.36 kHz). */
#include "hw_test.h"
#include "../../src/hw/via.h"

#define E MC_VIA_ECLOCK

static hw_fake f;
static mc_via v;

static uint8_t rd(unsigned reg) { return (uint8_t)mc_via_ops.read(&v, reg << 9, 1); }
static void wr(unsigned reg, uint8_t value) { mc_via_ops.write(&v, reg << 9, 1, value); }

static void fresh(void) {
    hw_fake_init(&f);
    mc_via_init(&v, &f.host, "VIA", 9);
    mc_via_ops.reset(&v);
}

static void test_ports(void) {
    fresh();
    /* Reset: everything input, no interrupt. */
    CHECK_EQ(hw_fake_value(&f, &v, MC_VIA_LINE_IRQ), -1);
    CHECK_EQ(hw_fake_value(&f, &v, MC_VIA_LINE_PA), 0xFF);
    mc_via_set_pa(&v, 0x5A, 0xFF);
    CHECK_EQ(rd(MC_VIA_ORA_NH), 0x5A);
    /* Outputs show ORA on their pins; inputs the pins. */
    wr(MC_VIA_DDRA, 0xF0);
    wr(MC_VIA_ORA, 0x3C);
    CHECK_EQ(hw_fake_value(&f, &v, MC_VIA_LINE_PA), 0x3F);   /* 0011 from ORA, inputs high */
    CHECK_EQ(rd(MC_VIA_ORA), 0x3A);
    CHECK_EQ(rd(MC_VIA_DDRA), 0xF0);
    /* Port B output bits read ORB, not the pins. */
    mc_via_set_pb(&v, 0x00, 0xFF);
    wr(MC_VIA_DDRB, 0x0F);
    wr(MC_VIA_ORB, 0xA5);
    CHECK_EQ(rd(MC_VIA_ORB), 0x05);
    CHECK_EQ(hw_fake_value(&f, &v, MC_VIA_LINE_PB), 0xF5);
    /* IER: bit 7 selects set or clear; it reads back with bit 7 set. */
    wr(MC_VIA_IER, 0x82);
    CHECK_EQ(rd(MC_VIA_IER), 0x82);
    wr(MC_VIA_IER, 0xC1);
    CHECK_EQ(rd(MC_VIA_IER), 0xC3);
    wr(MC_VIA_IER, 0x02);
    CHECK_EQ(rd(MC_VIA_IER), 0xC1);
}

static void test_timer1(void) {
    fresh();
    /* One-shot: 16 counts. */
    wr(MC_VIA_IER, 0xC0);
    wr(MC_VIA_T1CL, 16);
    wr(MC_VIA_T1CH, 0);
    CHECK_EQ(rd(MC_VIA_T1LL), 16);
    CHECK_EQ(mc_via_t1(&v), 16);
    f.now = 5 * E;
    CHECK_EQ(mc_via_t1(&v), 11);
    CHECK_EQ(hw_fake_deadline(&f, &v), 350);                  /* (16 + 1.5) x 20 */
    hw_fake_advance(&f, 349);
    CHECK_EQ(rd(MC_VIA_IFR) & 0x40, 0);
    hw_fake_advance(&f, 350);
    CHECK_EQ(rd(MC_VIA_IFR), 0xC0);
    CHECK_EQ(hw_fake_value(&f, &v, MC_VIA_LINE_IRQ), 1);
    /* It keeps counting down through $FFFF; reading T1C-L clears the flag. */
    f.now = 18 * E;
    CHECK_EQ(rd(MC_VIA_T1CH), 0xFF);
    CHECK_EQ(rd(MC_VIA_T1CL), 0xFE);
    CHECK_EQ(rd(MC_VIA_IFR), 0x00);
    CHECK_EQ(hw_fake_value(&f, &v, MC_VIA_LINE_IRQ), 0);
    /* One shot only. */
    CHECK_EQ(hw_fake_deadline(&f, &v), MC_NEVER);
    hw_fake_advance(&f, 100000);
    CHECK_EQ(rd(MC_VIA_IFR), 0x00);
    /* A write to T1L-H clears a flag too. */
    f.now = 200000;
    wr(MC_VIA_T1CH, 0);
    hw_fake_advance(&f, 200000 + 350);
    CHECK_EQ(rd(MC_VIA_IFR) & 0x40, 0x40);
    wr(MC_VIA_T1LH, 0);
    CHECK_EQ(rd(MC_VIA_IFR) & 0x40, 0);

    /* Free-running with PB7: N = 10, period 12 cycles, PB7 low at the load
     * and inverted at each flag. */
    fresh();
    wr(MC_VIA_ACR, 0xC0);
    wr(MC_VIA_T1CL, 10);
    wr(MC_VIA_T1CH, 0);
    CHECK_EQ(hw_fake_value(&f, &v, MC_VIA_LINE_PB) & 0x80, 0);
    CHECK_EQ(hw_fake_deadline(&f, &v), 230);                  /* 11.5 cycles */
    hw_fake_advance(&f, 230);
    CHECK_EQ(hw_fake_value(&f, &v, MC_VIA_LINE_PB) & 0x80, 0x80);
    CHECK_EQ(rd(MC_VIA_IFR) & 0x40, 0x40);
    CHECK_EQ(mc_via_t1(&v), 0xFFFF);            /* between the flag and the reload */
    f.now = 240;
    CHECK_EQ(mc_via_t1(&v), 10);                /* reloaded */
    f.now = 260;
    CHECK_EQ(mc_via_t1(&v), 9);
    CHECK_EQ(hw_fake_deadline(&f, &v), 470);
    hw_fake_advance(&f, 470);
    CHECK_EQ(hw_fake_value(&f, &v, MC_VIA_LINE_PB) & 0x80, 0);
    CHECK_EQ(rd(MC_VIA_ORB) & 0x80, 0);         /* PB7 reads the timer output */
    /* A new latch takes effect at the next reload. */
    wr(MC_VIA_T1LL, 20);
    hw_fake_advance(&f, 710);
    CHECK_EQ(hw_fake_value(&f, &v, MC_VIA_LINE_PB) & 0x80, 0x80);
    f.now = 720;
    CHECK_EQ(mc_via_t1(&v), 20);
    CHECK_EQ(hw_fake_deadline(&f, &v), 720 + 430);            /* 21.5 cycles */
    /* Many periods later the flag and PB7 are where they belong. */
    rd(MC_VIA_T1CL);
    wr(MC_VIA_ACR, 0x40);                       /* PB7 output off: nothing to schedule */
    CHECK_EQ(hw_fake_deadline(&f, &v), MC_NEVER);
    f.now = 720 + 22u * E * 1000u + 5u * E;
    CHECK_EQ(mc_via_t1(&v), 15);
    CHECK_EQ(rd(MC_VIA_IFR) & 0x40, 0x40);
}

static void test_timer2(void) {
    fresh();
    wr(MC_VIA_IER, 0xA0);
    wr(MC_VIA_T2CL, 5);
    wr(MC_VIA_T2CH, 0);
    CHECK_EQ(hw_fake_deadline(&f, &v), 130);                  /* 6.5 cycles */
    hw_fake_advance(&f, 130);
    CHECK_EQ(rd(MC_VIA_IFR), 0xA0);
    f.now = 10 * E;
    CHECK_EQ(rd(MC_VIA_T2CL), 0xFB);            /* still counting, flag cleared */
    CHECK_EQ(rd(MC_VIA_T2CH), 0xFF);
    CHECK_EQ(rd(MC_VIA_IFR), 0x00);
    hw_fake_advance(&f, 2000000);
    CHECK_EQ(rd(MC_VIA_IFR), 0x00);             /* once per T2C-H write */
    /* Pulse counting: negative edges on PB6. */
    fresh();
    wr(MC_VIA_ACR, 0x20);
    wr(MC_VIA_T2CL, 2);
    wr(MC_VIA_T2CH, 0);
    for (int i = 0; i < 2; i++) {
        mc_via_set_pb(&v, 0x00, 0x40);
        mc_via_set_pb(&v, 0x40, 0x40);
    }
    CHECK_EQ(mc_via_t2(&v), 0);
    CHECK_EQ(rd(MC_VIA_IFR) & 0x20, 0);
    mc_via_set_pb(&v, 0x00, 0x40);
    CHECK_EQ(rd(MC_VIA_IFR) & 0x20, 0x20);
    CHECK_EQ(mc_via_t2(&v), 0xFFFF);
}

static void test_shift(void) {
    fresh();
    /* Mode 7, shift out under external CB1 (the ADB transmit mode). */
    wr(MC_VIA_ACR, 0x1C);
    wr(MC_VIA_SR, 0xA5);
    CHECK_EQ(rd(MC_VIA_IFR) & 0x04, 0);
    wr(MC_VIA_IFR, 0x18);
    CHECK_EQ(mc_via_shift8(&v, 0x00), 0xA5);
    CHECK_EQ(rd(MC_VIA_IFR) & 0x18, 0); /* byte clocks are not control-line requests */
    CHECK_EQ(rd(MC_VIA_IFR) & 0x04, 0x04);
    CHECK_EQ(rd(MC_VIA_SR), 0xA5);              /* rotated back in; the read clears the flag */
    CHECK_EQ(rd(MC_VIA_IFR) & 0x04, 0);
    /* Mode 3, shift in under CB1 (the ADB receive mode). */
    wr(MC_VIA_ACR, 0x0C);
    rd(MC_VIA_SR);
    mc_via_shift8(&v, 0x3C);
    CHECK_EQ(rd(MC_VIA_IFR) & 0x04, 0x04);
    CHECK_EQ(rd(MC_VIA_SR), 0x3C);
    /* Mode 2, shift in at the E clock: 8 cycles after the access. */
    wr(MC_VIA_IER, 0x84);
    wr(MC_VIA_ACR, 0x08);
    mc_via_set_cb2(&v, 1);
    rd(MC_VIA_SR);
    CHECK_EQ(hw_fake_deadline(&f, &v), 8 * E);
    hw_fake_advance(&f, 8 * E);
    CHECK_EQ(hw_fake_value(&f, &v, MC_VIA_LINE_IRQ), 1);
    CHECK_EQ(rd(MC_VIA_SR), 0xFF);
    /* Mode 5, shift out at the Timer 2 rate: 8 x 2 x (N + 2) cycles. */
    f.now = 1000;
    wr(MC_VIA_T2CL, 3);
    wr(MC_VIA_ACR, 0x14);
    wr(MC_VIA_SR, 0x81);
    CHECK_EQ(hw_fake_deadline(&f, &v), 1000 + 8 * 2 * 5 * E);
    hw_fake_advance(&f, 1000 + 8 * 2 * 5 * E);
    CHECK_EQ(rd(MC_VIA_IFR) & 0x04, 0x04);
    CHECK_EQ(hw_fake_value(&f, &v, MC_VIA_LINE_CB2), 1);       /* the last bit, SR0 */
}

static void test_handshake(void) {
    fresh();
    /* CA1 negative edge (PCR0 = 0) sets its flag; reading ORA clears it. */
    wr(MC_VIA_IER, 0x82);
    mc_via_set_ca1(&v, 0);
    CHECK_EQ(rd(MC_VIA_IFR), 0x82);
    CHECK_EQ(hw_fake_value(&f, &v, MC_VIA_LINE_IRQ), 1);
    rd(MC_VIA_ORA_NH);                          /* register 15 leaves it */
    CHECK_EQ(rd(MC_VIA_IFR) & 0x02, 0x02);
    rd(MC_VIA_ORA);
    CHECK_EQ(rd(MC_VIA_IFR) & 0x02, 0);
    mc_via_set_ca1(&v, 1);                      /* the wrong edge */
    CHECK_EQ(rd(MC_VIA_IFR) & 0x02, 0);
    /* Positive edge selected. */
    wr(MC_VIA_PCR, 0x01);
    mc_via_set_ca1(&v, 0);
    CHECK_EQ(rd(MC_VIA_IFR) & 0x02, 0);
    mc_via_set_ca1(&v, 1);
    CHECK_EQ(rd(MC_VIA_IFR) & 0x02, 0x02);
    /* Input latching on CA1 (ACR0). */
    wr(MC_VIA_ACR, 0x01);
    mc_via_set_pa(&v, 0x11, 0xFF);
    mc_via_set_ca1(&v, 0);
    mc_via_set_ca1(&v, 1);
    mc_via_set_pa(&v, 0x22, 0xFF);
    CHECK_EQ(rd(MC_VIA_ORA), 0x11);
    wr(MC_VIA_ACR, 0x00);
    CHECK_EQ(rd(MC_VIA_ORA), 0x22);
    /* CA2 handshake output (PCR 100): low on an ORA access, high at the CA1 edge. */
    wr(MC_VIA_PCR, 0x09);
    CHECK_EQ(hw_fake_value(&f, &v, MC_VIA_LINE_CA2), -1);      /* still high: never reported */
    rd(MC_VIA_ORA);
    CHECK_EQ(hw_fake_value(&f, &v, MC_VIA_LINE_CA2), 0);
    mc_via_set_ca1(&v, 0);
    mc_via_set_ca1(&v, 1);
    CHECK_EQ(hw_fake_value(&f, &v, MC_VIA_LINE_CA2), 1);
    /* CB2 pulse output (PCR 101): one low pulse per ORB write. */
    const unsigned before = hw_fake_reports(&f, &v, MC_VIA_LINE_CB2);
    wr(MC_VIA_PCR, 0xA0);
    wr(MC_VIA_ORB, 0);
    CHECK_EQ(hw_fake_reports(&f, &v, MC_VIA_LINE_CB2) - before, 2);
    CHECK_EQ(hw_fake_value(&f, &v, MC_VIA_LINE_CB2), 1);
    /* CB2 manual low (110). */
    wr(MC_VIA_PCR, 0xC0);
    CHECK_EQ(hw_fake_value(&f, &v, MC_VIA_LINE_CB2), 0);
    /* CA2 independent input (001): an ORA access leaves its flag. */
    wr(MC_VIA_PCR, 0x02);
    mc_via_set_ca2(&v, 0);
    CHECK_EQ(rd(MC_VIA_IFR) & 0x01, 0x01);
    rd(MC_VIA_ORA);
    CHECK_EQ(rd(MC_VIA_IFR) & 0x01, 0x01);
    /* Writing 1s to IFR clears those flags. */
    wr(MC_VIA_IFR, 0x01);
    CHECK_EQ(rd(MC_VIA_IFR) & 0x01, 0);
    /* CB1 flag, cleared by an ORB access. */
    wr(MC_VIA_PCR, 0x10);
    mc_via_set_cb1(&v, 0);
    mc_via_set_cb1(&v, 1);
    CHECK_EQ(rd(MC_VIA_IFR) & 0x10, 0x10);
    rd(MC_VIA_ORB);
    CHECK_EQ(rd(MC_VIA_IFR) & 0x10, 0);
}

int mc_test_via(void) {
    test_ports();
    test_timer1();
    test_timer2();
    test_shift();
    test_handshake();
    return 0;
}
