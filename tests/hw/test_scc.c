/* test_scc: the Z85C30 model against the Zilog SCC/ESCC User's Manual
 * (1992): reset values (Table 2-7), the register pointer (2.3.4), the BRG
 * time constant (3.2) and asynchronous character timing (4.2), the receive
 * FIFO and overrun (5.3.2), interrupts and vector modification (2.4,
 * Table 2-9), external/status latching (2.4.9), local loopback and auto echo
 * (2.6), Macintosh register offsets (Inside Macintosh III: bCtl 0, aCtl 2,
 * bData 4, aData 6). */
#include "hw_test.h"
#include "scc.h"

static int tests;
#define T() (tests++)

enum { B_CTL = 0, A_CTL = 2, B_DATA = 4, A_DATA = 6 };

static hw_fake fake;
static mc_scc scc;
static uint8_t sent[2][64];
static uint64_t sent_at[2][64];
static unsigned nsent[2];
static unsigned outs[2], nouts;

static void on_tx(void *ctx, int ch, uint8_t b) {
    (void)ctx;
    if (nsent[ch] < 64) { sent_at[ch][nsent[ch]] = fake.now; sent[ch][nsent[ch]++] = b; }
}
static void on_out(void *ctx, int ch, unsigned o) { (void)ctx; outs[ch] = o; nouts++; }

static unsigned ctl(int ch) { return ch == MC_SCC_A ? A_CTL : B_CTL; }
static void wreg(int ch, unsigned reg, uint8_t v) {
    if (reg) scc.dev.ops->write(&scc, ctl(ch), 1, (reg & 7) | (reg >= 8 ? 0x08 : 0));
    scc.dev.ops->write(&scc, ctl(ch), 1, v);
}
static uint8_t rreg(int ch, unsigned reg) {
    if (reg) scc.dev.ops->write(&scc, ctl(ch), 1, (reg & 7) | (reg >= 8 ? 0x08 : 0));
    return (uint8_t)scc.dev.ops->read(&scc, ctl(ch), 1);
}
static void wdata(int ch, uint8_t v) { scc.dev.ops->write(&scc, ch == MC_SCC_A ? A_DATA : B_DATA, 1, v); }
static uint8_t rdata(int ch) { return (uint8_t)scc.dev.ops->read(&scc, ch == MC_SCC_A ? A_DATA : B_DATA, 1); }
static int irq(void) { return hw_fake_line(&fake, &scc, MC_SCC_LINE_IRQ); }

static void fresh(void) {
    hw_fake_init(&fake);
    mc_scc_init(&scc, &fake.host);
    mc_scc_port port = {NULL, on_tx, on_out};
    mc_scc_set_port(&scc, &port);
    memset(nsent, 0, sizeof nsent);
    nouts = 0;
    outs[0] = outs[1] = 0;
}
/* Asynchronous, x16 clock, `bits` (WR5/WR3 code), stop/parity in WR4, BRG
 * from PCLK with time constant `tc`, Rx and Tx enabled. The Mac OS serial
 * driver's sequence (SCC UM Table 4-3). */
static void async_setup(int ch, uint8_t wr4, uint8_t bitcode, unsigned tc) {
    wreg(ch, 4, wr4);
    wreg(ch, 3, (uint8_t)(bitcode << 6));
    wreg(ch, 5, (uint8_t)(bitcode << 5 | 0x82));   /* DTR, RTS */
    wreg(ch, 11, 0x50);                        /* Rx and Tx clocks from the BRG */
    wreg(ch, 12, (uint8_t)tc);
    wreg(ch, 13, (uint8_t)(tc >> 8));
    wreg(ch, 14, 0x03);                        /* BRG on, from PCLK */
    wreg(ch, 3, (uint8_t)(bitcode << 6 | 1));
    wreg(ch, 5, (uint8_t)(bitcode << 5 | 0x8A));
}

static void test_reset_and_pointer(void) {
    fresh();
    /* Table 2-7: RR0 x1xxx100 (with DCD and CTS asserted: D3, D5), RR1
       0000011x (all sent while idle), RR3 0, RR10 0, RR15 = WR15 11111000. */
    CHECK_EQ(rreg(MC_SCC_A, 0), 0x6C); T();
    CHECK_EQ(rreg(MC_SCC_B, 0), 0x6C); T();
    CHECK_EQ(rreg(MC_SCC_A, 1), 0x07); T();
    CHECK_EQ(rreg(MC_SCC_A, 3), 0); T();
    CHECK_EQ(rreg(MC_SCC_A, 10), 0); T();
    CHECK_EQ(rreg(MC_SCC_B, 15), 0xF8); T();
    CHECK_EQ(irq(), 0); T();
    /* The pointer selects one register for one access, then returns to 0
       (2.3.4); Point High adds 8; the pointer is shared by both channels. */
    wreg(MC_SCC_A, 12, 0x5A);
    CHECK_EQ(rreg(MC_SCC_A, 12), 0x5A); T();
    CHECK_EQ((uint8_t)scc.dev.ops->read(&scc, A_CTL, 1), 0x6C); T();   /* back on RR0 */
    scc.dev.ops->write(&scc, B_CTL, 1, 0x08 | 5);   /* pointer 13 set through channel B */
    CHECK_EQ(scc.ptr, 13); T();
    scc.dev.ops->write(&scc, A_CTL, 1, 0x12);       /* ... used by channel A */
    CHECK_EQ(scc.ch[MC_SCC_A].wr[13], 0x12); T();
    CHECK_EQ(scc.ptr, 0); T();
    /* The data registers are addressed directly and leave the pointer
       alone (2.3.4). */
    scc.dev.ops->write(&scc, A_CTL, 1, 2);
    rdata(MC_SCC_A);
    CHECK_EQ(scc.ptr, 2); T();
    /* RR12/RR13 read back the time constant; RR9 and RR11 alias RR13 and
       RR15 on the Z85C30 (Table 5-3). */
    CHECK_EQ(rreg(MC_SCC_A, 0), 0x00); T();   /* the pointer still selects RR2: the vector, 0 */
    CHECK_EQ(rreg(MC_SCC_A, 9), 0x12); T();
    CHECK_EQ(rreg(MC_SCC_A, 11), 0xF8); T();
    /* WR2 is one register for both channels; RR2 of channel A reads it. */
    wreg(MC_SCC_B, 2, 0x40);
    CHECK_EQ(rreg(MC_SCC_A, 2), 0x40); T();
}

static void test_timing(void) {
    fresh();
    /* 8 bits, 1 stop, no parity, x16; TC 10 from 3.672 MHz: one bit is
       2 x (10 + 2) x 16 = 384 PCLK cycles (3.2), a character 10 bits =
       3840 cycles = 3840 x 64/15 = 16384 C15M periods. */
    async_setup(MC_SCC_A, 0x44, 3, 10);
    CHECK_EQ(outs[MC_SCC_A], MC_SCC_OUT_DTR | MC_SCC_OUT_RTS); T();
    wdata(MC_SCC_A, 0x41);
    /* The buffer went straight into the shift register: buffer empty again. */
    CHECK_EQ(rreg(MC_SCC_A, 0) & 0x04, 0x04); T();
    CHECK_EQ(rreg(MC_SCC_A, 1) & 0x01, 0); T();   /* not all sent */
    wdata(MC_SCC_A, 0x42);
    CHECK_EQ(rreg(MC_SCC_A, 0) & 0x04, 0); T();    /* buffer full */
    hw_fake_step(&fake, 16383);
    CHECK_EQ(nsent[MC_SCC_A], 0); T();
    hw_fake_step(&fake, 1);
    CHECK_EQ(nsent[MC_SCC_A], 1); T();
    CHECK_EQ(sent[MC_SCC_A][0], 0x41); T();
    CHECK_EQ(rreg(MC_SCC_A, 0) & 0x04, 0x04); T();
    hw_fake_step(&fake, 16384);
    CHECK_EQ(nsent[MC_SCC_A], 2); T();
    CHECK_EQ(sent_at[MC_SCC_A][1], 32768); T();
    CHECK_EQ(rreg(MC_SCC_A, 1) & 0x01, 1); T();   /* all sent */

    /* 7 bits, even parity, 2 stop bits, x16, TC 4: bit = 2 x 6 x 16 = 192
       cycles; 1 + 7 + 1 + 2 = 11 bits = 2112 cycles = 9011.2 C15M periods
       (the fraction carried over). The byte leaves masked to 7 bits. */
    fresh();
    async_setup(MC_SCC_B, 0x4F, 1, 4);
    uint64_t t0 = fake.now;
    wdata(MC_SCC_B, 0xC1);
    hw_fake_step(&fake, 9011);
    CHECK_EQ(nsent[MC_SCC_B], 1); T();
    CHECK_EQ(sent[MC_SCC_B][0], 0x41); T();
    CHECK_EQ(sent_at[MC_SCC_B][0] - t0, 9011); T();
    /* 1.5 stop bits, 5 bits, x64, TC 0: bit = 2 x 2 x 64 = 256 cycles;
       1 + 5 + 1.5 = 7.5 bits = 1920 cycles = 8192 C15M periods. */
    fresh();
    async_setup(MC_SCC_A, 0xC8, 0, 0);
    wdata(MC_SCC_A, 0xFF);
    hw_fake_step(&fake, 8192);
    CHECK_EQ(nsent[MC_SCC_A], 1); T();
    CHECK_EQ(sent[MC_SCC_A][0], 0x1F); T();
    /* x1 clock from the RTxC pin (WR11 Tx clock 00): one bit per RTxC
       cycle at 3.672 MHz; 10 bits = 10 x 64/15 = 42.67 periods. */
    fresh();
    async_setup(MC_SCC_A, 0x04, 3, 0);
    wreg(MC_SCC_A, 11, 0x00);
    wdata(MC_SCC_A, 0x55);
    hw_fake_step(&fake, 42);
    CHECK_EQ(nsent[MC_SCC_A], 1); T();
    /* The BRG disabled stops a BRG-clocked transmitter. */
    fresh();
    async_setup(MC_SCC_A, 0x44, 3, 10);
    wreg(MC_SCC_A, 14, 0x02);
    wdata(MC_SCC_A, 0x55);
    hw_fake_step(&fake, 1000000);
    CHECK_EQ(nsent[MC_SCC_A], 0); T();
    wreg(MC_SCC_A, 14, 0x03);
    hw_fake_step(&fake, 16384);
    CHECK_EQ(nsent[MC_SCC_A], 1); T();
}

static void test_receive(void) {
    fresh();
    async_setup(MC_SCC_A, 0x44, 3, 10);
    uint8_t in[5] = {1, 2, 3, 4, 5};
    CHECK_EQ(mc_scc_receive(&scc, MC_SCC_A, in, 1), 1); T();
    hw_fake_step(&fake, 16383);
    CHECK_EQ(rreg(MC_SCC_A, 0) & 1, 0); T();
    hw_fake_step(&fake, 1);
    CHECK_EQ(rreg(MC_SCC_A, 0) & 1, 1); T();
    CHECK_EQ(rdata(MC_SCC_A), 1); T();
    CHECK_EQ(rreg(MC_SCC_A, 0) & 1, 0); T();
    /* Five characters with nobody reading: the 3-byte FIFO keeps the first
       two and the last, which overwrote the third and fourth and carries
       the overrun flag (5.3.2). */
    mc_scc_receive(&scc, MC_SCC_A, in, 5);
    hw_fake_step(&fake, 5 * 16384);
    CHECK_EQ(rreg(MC_SCC_A, 1) & 0x20, 0); T();
    CHECK_EQ(rdata(MC_SCC_A), 1); T();
    CHECK_EQ(rdata(MC_SCC_A), 2); T();
    CHECK_EQ(rreg(MC_SCC_A, 1) & 0x20, 0x20); T();   /* the exit character overran */
    CHECK_EQ(rdata(MC_SCC_A), 5); T();
    CHECK_EQ(rreg(MC_SCC_A, 0) & 1, 0); T();
    CHECK_EQ(rreg(MC_SCC_A, 1) & 0x20, 0x20); T();   /* latched */
    wreg(MC_SCC_A, 0, 0x30);                            /* Error Reset */
    CHECK_EQ(rreg(MC_SCC_A, 1) & 0x20, 0); T();
    /* 7 bits with odd parity: the parity bit is assembled above the data
       and the unused high bit reads 1 (4.2.2). 0x41 has two 1s: odd parity
       bit 1. */
    fresh();
    async_setup(MC_SCC_B, 0x45, 1, 10);
    uint8_t a = 0x41;
    mc_scc_receive(&scc, MC_SCC_B, &a, 1);
    hw_fake_step(&fake, 20000);
    CHECK_EQ(rdata(MC_SCC_B), 0xC1); T();
    /* Receiver disabled: characters on the line are lost. */
    fresh();
    async_setup(MC_SCC_B, 0x44, 3, 10);
    wreg(MC_SCC_B, 3, 0xC0);
    mc_scc_receive(&scc, MC_SCC_B, &a, 1);
    hw_fake_step(&fake, 20000);
    CHECK_EQ(rreg(MC_SCC_B, 0) & 1, 0); T();
    CHECK_EQ(mc_scc_receive_room(&scc, MC_SCC_B), MC_SCC_LINE_QUEUE); T();
}

/* Raises one interrupt source of one channel (Figure 2-9). */
static void source(int ch, int kind) {
    switch (kind) {
    case 0:   /* Tx buffer empty */
        wreg(ch, 1, 0x02);
        wdata(ch, 0x33);
        break;
    case 1:   /* External/status: DCD */
        wreg(ch, 15, 0x08);
        wreg(ch, 1, 0x01);
        mc_scc_set_dcd(&scc, ch, 0);
        break;
    case 2: { /* Rx character available */
        uint8_t b = 7;
        wreg(ch, 1, 0x10);
        mc_scc_receive(&scc, ch, &b, 1);
        hw_fake_step(&fake, 16384);
        break;
    }
    default: { /* Special receive condition: an overrun at the exit */
        uint8_t b[4] = {1, 2, 3, 4};
        wreg(ch, 1, 0x10);
        mc_scc_receive(&scc, ch, b, 4);
        hw_fake_step(&fake, 4 * 16384);
        rdata(ch);
        rdata(ch);
        break;
    }
    }
}

static void test_vectors(void) {
    /* Table 2-9: B Tx 000, B Ext 001, B Rx 010, B special 011, A Tx 100,
       A Ext 101, A Rx 110, A special 111; status low in V3-V1, status high
       in V4-V6 with the code's high bit in V4. */
    for (int high = 0; high < 2; high++) {
        for (int code = 0; code < 8; code++) {
            fresh();
            async_setup(MC_SCC_A, 0x44, 3, 10);
            async_setup(MC_SCC_B, 0x44, 3, 10);
            wreg(MC_SCC_A, 2, 0x81);
            wreg(MC_SCC_A, 9, (uint8_t)(0x08 | (high ? 0x10 : 0) | 0x01));   /* MIE, VIS */
            CHECK_EQ(irq(), 0); T();
            source(code & 4 ? MC_SCC_A : MC_SCC_B, code & 3);
            CHECK_EQ(irq(), 1); T();
            uint8_t want;
            if (high) want = (uint8_t)(0x81 | ((code & 4) ? 0x10 : 0) | ((code & 2) ? 0x20 : 0) | ((code & 1) ? 0x40 : 0));
            else want = (uint8_t)(0x81 | code << 1);
            CHECK_EQ(rreg(MC_SCC_B, 2), want); T();
            CHECK_EQ(rreg(MC_SCC_A, 2), 0x81); T();   /* channel A: unmodified */
            /* A hardware acknowledge returns the same modified vector (VIS). */
            CHECK_EQ(mc_scc_iack(&scc), want); T();
            CHECK_EQ(irq(), 0); T();   /* under service */
            wreg(MC_SCC_A, 0, 0x38);   /* Reset Highest IUS */
        }
    }
    /* Nothing pending: RR2B carries the channel B special-condition code
       011. NV: no vector on acknowledge. */
    fresh();
    wreg(MC_SCC_A, 2, 0x00);
    CHECK_EQ(rreg(MC_SCC_B, 2), 0x06); T();
    CHECK_EQ(mc_scc_iack(&scc), -1); T();
}

static void test_priority(void) {
    fresh();
    async_setup(MC_SCC_A, 0x44, 3, 10);
    async_setup(MC_SCC_B, 0x44, 3, 10);
    wreg(MC_SCC_A, 9, 0x08);   /* MIE, status low, VIS off */
    /* B Tx and A Rx pending: RR3 (channel A) shows both IPs (Figure 2-12);
       A Rx outranks B Tx. */
    source(MC_SCC_B, 0);
    source(MC_SCC_A, 2);
    CHECK_EQ(rreg(MC_SCC_A, 3), 0x22); T();
    CHECK_EQ(rreg(MC_SCC_B, 3), 0); T();
    CHECK_EQ(rreg(MC_SCC_B, 2) & 0x0E, 0x0C); T();
    /* Software acknowledge (WR9 D5): reading RR2B sets the IUS of A Rx,
       which masks B Tx: the line drops although both are pending. */
    wreg(MC_SCC_A, 9, 0x28);
    CHECK_EQ(rreg(MC_SCC_B, 2) & 0x0E, 0x0C); T();
    CHECK_EQ(irq(), 0); T();
    /* Clearing A Rx and resetting its IUS lets B Tx through. */
    rdata(MC_SCC_A);
    CHECK_EQ(irq(), 0); T();
    wreg(MC_SCC_A, 0, 0x38);
    CHECK_EQ(irq(), 1); T();
    CHECK_EQ(rreg(MC_SCC_B, 2) & 0x0E, 0x00); T();   /* acknowledged too (WR9 D5) */
    wreg(MC_SCC_B, 0, 0x28);   /* Reset Tx Int Pending */
    wreg(MC_SCC_B, 0, 0x38);
    CHECK_EQ(irq(), 0); T();
    CHECK_EQ(rreg(MC_SCC_A, 3), 0); T();
    /* MIE off: no interrupt, IPs still visible in RR3 (2.4.7.2). */
    wreg(MC_SCC_A, 9, 0x00);
    source(MC_SCC_A, 2);
    CHECK_EQ(irq(), 0); T();
    CHECK_EQ(rreg(MC_SCC_A, 3), 0x20); T();
    /* Rx interrupt on first character (WR1 01): one IP until the next
       Enable Int on Next Rx Character command. */
    fresh();
    async_setup(MC_SCC_A, 0x44, 3, 10);
    wreg(MC_SCC_A, 9, 0x08);
    wreg(MC_SCC_A, 1, 0x08);
    uint8_t two[2] = {1, 2};
    mc_scc_receive(&scc, MC_SCC_A, two, 2);
    hw_fake_step(&fake, 2 * 16384);
    CHECK_EQ(irq(), 1); T();
    rdata(MC_SCC_A);
    CHECK_EQ(irq(), 0); T();   /* the second character does not interrupt */
    wreg(MC_SCC_A, 0, 0x20);   /* Enable Int on Next Rx Character */
    CHECK_EQ(irq(), 1); T();
    /* Tx IP: set only when a full buffer empties, cleared by Reset Tx Int
       Pending (2.4.8). */
    fresh();
    async_setup(MC_SCC_A, 0x44, 3, 10);
    wreg(MC_SCC_A, 9, 0x08);
    wreg(MC_SCC_A, 1, 0x02);
    CHECK_EQ(irq(), 0); T();
    wdata(MC_SCC_A, 1);
    CHECK_EQ(irq(), 1); T();
    wreg(MC_SCC_A, 0, 0x28);
    CHECK_EQ(irq(), 0); T();
    hw_fake_step(&fake, 16384);
    CHECK_EQ(irq(), 0); T();   /* the shift register emptying is not a buffer-empty transition */
}

static void test_ext_status(void) {
    fresh();
    async_setup(MC_SCC_A, 0x44, 3, 10);
    wreg(MC_SCC_A, 9, 0x08);
    wreg(MC_SCC_A, 15, 0x28);   /* DCD and CTS interrupts */
    wreg(MC_SCC_A, 1, 0x01);
    mc_scc_set_cts(&scc, MC_SCC_A, 0);
    CHECK_EQ(irq(), 1); T();
    CHECK_EQ(rreg(MC_SCC_A, 3), 0x08); T();
    CHECK_EQ(rreg(MC_SCC_A, 0) & 0x28, 0x08); T();   /* latched: CTS 0, DCD 1 */
    mc_scc_set_dcd(&scc, MC_SCC_A, 0);
    mc_scc_set_cts(&scc, MC_SCC_A, 1);
    CHECK_EQ(rreg(MC_SCC_A, 0) & 0x28, 0x08); T();   /* still the latched value */
    wreg(MC_SCC_A, 0, 0x10);                          /* Reset Ext/Status Interrupts */
    CHECK_EQ(irq(), 0); T();
    CHECK_EQ(rreg(MC_SCC_A, 0) & 0x28, 0x20); T();   /* live again */
    /* A source whose WR15 enable is clear is not latched and does not
       interrupt (2.4.9). */
    wreg(MC_SCC_A, 15, 0x08);
    mc_scc_set_cts(&scc, MC_SCC_A, 0);
    CHECK_EQ(irq(), 0); T();
    CHECK_EQ(rreg(MC_SCC_A, 0) & 0x20, 0); T();
    /* Without the WR1 D0 master enable the latches still close but no IP. */
    wreg(MC_SCC_A, 1, 0x00);
    mc_scc_set_dcd(&scc, MC_SCC_A, 1);
    CHECK_EQ(irq(), 0); T();
    CHECK_EQ(rreg(MC_SCC_A, 3), 0); T();
    wreg(MC_SCC_A, 0, 0x10);
    /* Break: RR0 D7 while it lasts; the end leaves a null in the FIFO. */
    wreg(MC_SCC_A, 15, 0x80);
    wreg(MC_SCC_A, 1, 0x01);
    mc_scc_set_break(&scc, MC_SCC_A, 1);
    CHECK_EQ(irq(), 1); T();
    CHECK_EQ(rreg(MC_SCC_A, 0) & 0x80, 0x80); T();
    wreg(MC_SCC_A, 0, 0x10);
    mc_scc_set_break(&scc, MC_SCC_A, 0);
    CHECK_EQ(rreg(MC_SCC_A, 0) & 0x81, 0x01); T();
    CHECK_EQ(rdata(MC_SCC_A), 0); T();
    /* BRG zero count: an external/status interrupt every TC + 2 clocks. */
    fresh();
    async_setup(MC_SCC_B, 0x44, 3, 1000);
    wreg(MC_SCC_A, 9, 0x08);
    wreg(MC_SCC_B, 15, 0x02);
    wreg(MC_SCC_B, 1, 0x01);
    hw_fake_step(&fake, 1002 * 64 / 15);
    CHECK_EQ(irq(), 0); T();
    hw_fake_step(&fake, 1);
    CHECK_EQ(irq(), 1); T();
    CHECK_EQ(rreg(MC_SCC_B, 2) & 0x0E, 0x02); T();   /* B ext/status */
    /* Auto Enables: the transmitter waits for CTS (5.2.4). */
    fresh();
    async_setup(MC_SCC_A, 0x44, 3, 10);
    wreg(MC_SCC_A, 3, 0xE1);
    mc_scc_set_cts(&scc, MC_SCC_A, 0);
    wdata(MC_SCC_A, 9);
    hw_fake_step(&fake, 50000);
    CHECK_EQ(nsent[MC_SCC_A], 0); T();
    mc_scc_set_cts(&scc, MC_SCC_A, 1);
    hw_fake_step(&fake, 16384);
    CHECK_EQ(nsent[MC_SCC_A], 1); T();
}

static void test_loopback_and_resets(void) {
    /* Local loopback (WR14 D4): the transmitter feeds the receiver and TxD
       still carries the character (2.6.1). */
    fresh();
    async_setup(MC_SCC_A, 0x44, 3, 10);
    wreg(MC_SCC_A, 14, 0x13);
    wdata(MC_SCC_A, 0x5A);
    hw_fake_step(&fake, 16384);
    CHECK_EQ(nsent[MC_SCC_A], 1); T();
    CHECK_EQ(rdata(MC_SCC_A), 0x5A); T();
    /* Auto echo (D3): RxD goes out on TxD; the transmitter is disconnected
       (2.6.2). */
    fresh();
    async_setup(MC_SCC_A, 0x44, 3, 10);
    wreg(MC_SCC_A, 14, 0x0B);
    uint8_t b = 0x77;
    mc_scc_receive(&scc, MC_SCC_A, &b, 1);
    hw_fake_step(&fake, 16384);
    CHECK_EQ(nsent[MC_SCC_A], 1); T();
    CHECK_EQ(sent[MC_SCC_A][0], 0x77); T();
    CHECK_EQ(rdata(MC_SCC_A), 0x77); T();
    wdata(MC_SCC_A, 0x11);
    hw_fake_step(&fake, 16384);
    CHECK_EQ(nsent[MC_SCC_A], 1); T();
    /* DTR and RTS follow WR5 D7 and D1. */
    wreg(MC_SCC_A, 5, 0x68);
    CHECK_EQ(mc_scc_outputs(&scc, MC_SCC_A), 0); T();
    wreg(MC_SCC_A, 5, 0x78);
    CHECK_EQ(mc_scc_outputs(&scc, MC_SCC_A), MC_SCC_OUT_BREAK); T();
    /* Channel reset A (WR9 10) leaves channel B as it was; Table 2-7
       channel values: WR15 F8, WR5 DTR/RTS/Tx enable cleared. */
    async_setup(MC_SCC_B, 0x44, 3, 10);
    wreg(MC_SCC_A, 15, 0x00);
    wreg(MC_SCC_B, 15, 0x00);
    wreg(MC_SCC_A, 9, 0x80);
    CHECK_EQ(rreg(MC_SCC_A, 15), 0xF8); T();
    CHECK_EQ(rreg(MC_SCC_B, 15), 0x00); T();
    CHECK_EQ(mc_scc_outputs(&scc, MC_SCC_A), 0); T();
    CHECK_EQ(mc_scc_outputs(&scc, MC_SCC_B), MC_SCC_OUT_DTR | MC_SCC_OUT_RTS); T();
    CHECK_EQ(scc.ch[MC_SCC_A].wr[14] & 0x3C, 0x20); T();
    /* Force hardware reset (WR9 11): both channels, WR11 08, WR14 xx110000. */
    wreg(MC_SCC_B, 9, 0xC0);
    CHECK_EQ(rreg(MC_SCC_B, 15), 0xF8); T();
    CHECK_EQ(scc.ch[MC_SCC_B].wr[11], 0x08); T();
    CHECK_EQ(scc.ch[MC_SCC_B].wr[14] & 0x3F, 0x30); T();
    CHECK_EQ(mc_scc_outputs(&scc, MC_SCC_B), 0); T();
    CHECK_EQ(scc.ch[MC_SCC_A].wr[9] & 0x3C, 0); T();
    /* The device reset op is the hardware reset too. */
    wreg(MC_SCC_A, 12, 0x99);
    scc.dev.ops->reset(&scc);
    CHECK_EQ(rreg(MC_SCC_A, 12), 0x99); T();   /* WR12 unchanged (Table 2-7) */
    CHECK_EQ(rreg(MC_SCC_A, 0), 0x6C); T();
    /* /W//REQ in request mode, on receive (WR1 E0) and on transmit (C0). */
    fresh();
    async_setup(MC_SCC_A, 0x44, 3, 10);
    wreg(MC_SCC_A, 1, 0xE0);
    CHECK_EQ(hw_fake_line(&fake, &scc, MC_SCC_LINE_WREQ), 0); T();
    mc_scc_receive(&scc, MC_SCC_A, &b, 1);
    hw_fake_step(&fake, 16384);
    CHECK_EQ(hw_fake_line(&fake, &scc, MC_SCC_LINE_WREQ), 1); T();
    CHECK_EQ(hw_fake_line(&fake, &scc, MC_SCC_LINE_WREQ_A), 1); T();
    rdata(MC_SCC_A);
    CHECK_EQ(hw_fake_line(&fake, &scc, MC_SCC_LINE_WREQ), 0); T();
    wreg(MC_SCC_B, 1, 0xC0);
    CHECK_EQ(hw_fake_line(&fake, &scc, MC_SCC_LINE_WREQ_B), 1); T();
    /* Synchronous mode: registers accepted, bytes consumed without frames. */
    fresh();
    wreg(MC_SCC_A, 4, 0x20);   /* SDLC, x1 */
    wreg(MC_SCC_A, 11, 0x50);
    wreg(MC_SCC_A, 14, 0x03);
    wreg(MC_SCC_A, 5, 0x68);
    wdata(MC_SCC_A, 0x7E);
    hw_fake_step(&fake, 100000);
    CHECK_EQ(nsent[MC_SCC_A], 0); T();
    CHECK_EQ(rreg(MC_SCC_A, 0) & 0x44, 0x44); T();   /* buffer empty, underrun */
}

static void test_dma_data_ports(void) {
    fresh();
    async_setup(MC_SCC_A, 0x44, 3, 10);
    CHECK_EQ(hw_fake_line(&fake, &scc, MC_SCC_LINE_TX_READY_A), 1); T();
    CHECK_EQ(hw_fake_line(&fake, &scc, MC_SCC_LINE_WREQ_A), 0); T();
    scc.dev.ops->write(&scc, A_CTL, 1, 12);
    CHECK(mc_scc_dma_write(&scc, MC_SCC_A, 0x12)); T();
    CHECK(mc_scc_dma_write(&scc, MC_SCC_A, 0x34)); T();
    CHECK_EQ(scc.ptr, 12); T();
    CHECK_EQ(hw_fake_line(&fake, &scc, MC_SCC_LINE_TX_READY_A), 0); T();
    CHECK(!mc_scc_dma_write(&scc, MC_SCC_A, 0x56)); T();
    hw_fake_step(&fake, 16384);
    CHECK_EQ(hw_fake_line(&fake, &scc, MC_SCC_LINE_TX_READY_A), 1); T();
    CHECK(mc_scc_dma_write(&scc, MC_SCC_A, 0x56)); T();
    hw_fake_step(&fake, 32768);
    CHECK_EQ(nsent[MC_SCC_A], 3); T();
    CHECK_EQ(sent[MC_SCC_A][0], 0x12); T();
    CHECK_EQ(sent[MC_SCC_A][1], 0x34); T();
    CHECK_EQ(sent[MC_SCC_A][2], 0x56); T();
    uint8_t byte = 0;
    CHECK(!mc_scc_dma_read(&scc, MC_SCC_A, &byte)); T();
    const uint8_t incoming = 0xAB;
    CHECK_EQ(mc_scc_receive(&scc, MC_SCC_A, &incoming, 1), 1); T();
    hw_fake_step(&fake, 16384);
    CHECK_EQ(hw_fake_line(&fake, &scc, MC_SCC_LINE_RX_READY_A), 1); T();
    CHECK(mc_scc_dma_read(&scc, MC_SCC_A, &byte)); T();
    CHECK_EQ(byte, 0xAB); T();
    CHECK_EQ(scc.ptr, 12); T();
    CHECK_EQ(hw_fake_line(&fake, &scc, MC_SCC_LINE_RX_READY_A), 0); T();
}

int main(void) {
    test_reset_and_pointer();
    test_timing();
    test_receive();
    test_vectors();
    test_priority();
    test_ext_status();
    test_loopback_and_resets();
    test_dma_data_ports();
    printf("test_scc: %d tests, %d failure(s)\n", tests, mc_test_failures);
    return mc_test_failures ? 1 : 0;
}
