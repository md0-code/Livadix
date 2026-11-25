/* test_sonic: the DP83932 stub against the DP83932B datasheet (National,
 * Local Area Network Databook, 1992): hardware and software reset values
 * (4.3.1-4.3.13), the two-write command protocol in reset mode (4.3.1),
 * reset-only registers (Table 4-1 notes), the ISR/IMR interrupt (4.3.5,
 * 4.3.6), Load CAM (4.1.1), Read RRA (3.4.1), Transmit Packets on the TDA
 * (3.5.1) in 16- and 32-bit modes with fragments gathered and handed to
 * the network, PMB (4.3.4), reception through the RRA, RBA and RDA with
 * its descriptors, sequence numbers, EOBC and the overflow conditions RDE,
 * RBE, RBAE (3.4), the address filter (CAM, BRD, AMC, PRO, RJCM), the
 * wire's timing, MAC loopback (1.7), tally counters (4.3.11), the general
 * purpose timer (4.3.12), and the Ethernet address PROM. */
#include "hw_test.h"
#include "sonic.h"

static int tests;
#define T() (tests++)

static hw_fake fake;
static mc_sonic sonic;
static uint8_t mem[0x10000];   /* physical $00xx0000-$00xxFFFF, upper half ignored */

/* The host's bus-master accessor (hw.h) on mem. */
static int t_dma(void *ctx, uint32_t a, void *data, uint32_t length, int write) {
    (void)ctx;
    uint8_t *p = data;
    for (uint32_t k = 0; k < length; k++) {
        if (write) mem[(a + k) & 0xFFFF] = p[k];
        else p[k] = mem[(a + k) & 0xFFFF];
    }
    return 1;
}
static void poke(uint32_t a, uint16_t v) { mem[a & 0xFFFF] = (uint8_t)(v >> 8); mem[(a + 1) & 0xFFFF] = (uint8_t)v; }
static uint16_t peek(uint32_t a) { return (uint16_t)(mem[a & 0xFFFF] << 8 | mem[(a + 1) & 0xFFFF]); }

/* Registers at 32-bit spacing, as on the Quadra 700. */
static void w(unsigned reg, uint16_t v) { sonic.dev.ops->write(&sonic, reg << 2, 4, v); }
static uint16_t r(unsigned reg) { return (uint16_t)sonic.dev.ops->read(&sonic, reg << 2, 4); }
static int irq(void) { return hw_fake_line(&fake, &sonic, MC_SONIC_LINE_IRQ); }

/* The network: what the SONIC transmitted. */
static uint8_t sent[4][1600];
static size_t sent_length[4];
static unsigned sent_count;
static void on_transmit(void *ctx, const uint8_t *frame, size_t length) {
    (void)ctx;
    if (sent_count < 4) {
        memcpy(sent[sent_count], frame, length);
        sent_length[sent_count] = length;
    }
    sent_count++;
}

static void fresh(int with_dma) {
    hw_fake_init(&fake);
    if (with_dma) fake.host.dma = t_dma;
    mc_sonic_init(&sonic, &fake.host, 2);
    memset(mem, 0, sizeof mem);
    sent_count = 0;
}
static void plug(void) {
    const mc_sonic_port port = {NULL, on_transmit};
    mc_sonic_set_port(&sonic, &port);
}

static void test_reset(void) {
    fresh(0);
    CHECK_EQ(r(MC_SONIC_CR), 0x0094); T();      /* 4.3.1: RST, STP, RXDIS */
    CHECK_EQ(r(MC_SONIC_DCR) & 0xA000, 0); T(); /* 4.3.2 */
    CHECK_EQ(r(MC_SONIC_TCR), 0x0102); T();     /* 4.3.4: NCRS, BCM */
    CHECK_EQ(r(MC_SONIC_IMR), 0); T();
    CHECK_EQ(r(MC_SONIC_ISR), 0); T();
    CHECK_EQ(r(MC_SONIC_DCR2), 0); T();
    CHECK_EQ(r(MC_SONIC_EOBC), 0x02F8); T();    /* 4.3.9: 760 words */
    CHECK_EQ(r(MC_SONIC_RSC), 0); T();
    CHECK_EQ(r(MC_SONIC_CE), 0); T();
    CHECK_EQ(r(MC_SONIC_SR), MC_SONIC_REVISION); T();
    CHECK_EQ(irq(), 0); T();
    /* Byte and word accesses read the same 16 bits. */
    CHECK_EQ(sonic.dev.ops->read(&sonic, (MC_SONIC_CR << 2) + 2, 2), 0x0094); T();
    CHECK_EQ(sonic.dev.ops->read(&sonic, (MC_SONIC_CR << 2) + 3, 1), 0x94); T();
    /* DCR and DCR2 take writes only in reset mode (Table 4-1, note 3). */
    w(MC_SONIC_DCR, 0x003F);
    w(MC_SONIC_DCR2, 0x1234);
    CHECK_EQ(r(MC_SONIC_DCR), 0x003F); T();
    CHECK_EQ(r(MC_SONIC_DCR2), 0x1234); T();
    /* In reset mode a command write only clears RST (4.3.1). */
    w(MC_SONIC_CR, MC_SONIC_CR_TXP);
    CHECK_EQ(r(MC_SONIC_CR), 0x0014); T();
    CHECK_EQ(r(MC_SONIC_ISR), 0); T();
    w(MC_SONIC_DCR, 0x0000);
    CHECK_EQ(r(MC_SONIC_DCR), 0x003F); T();
    /* RXEN and RXDIS exclude each other. */
    w(MC_SONIC_CR, MC_SONIC_CR_RXEN);
    CHECK_EQ(r(MC_SONIC_CR) & 0x000C, MC_SONIC_CR_RXEN); T();
    /* Software reset: bits 9, 8, 1, 0 cleared, 7 and 2 set; IMR, TCR,
       DCR unaffected (4.3.1-4.3.5). */
    w(MC_SONIC_IMR, 0x0200);
    w(MC_SONIC_CR, MC_SONIC_CR_RST);
    CHECK_EQ(r(MC_SONIC_CR) & 0x0387, 0x0084); T();
    CHECK_EQ(r(MC_SONIC_IMR), 0x0200); T();
    CHECK_EQ(r(MC_SONIC_DCR), 0x003F); T();
    /* Hardware reset through the device op clears IMR. */
    sonic.dev.ops->reset(&sonic);
    CHECK_EQ(r(MC_SONIC_IMR), 0); T();
    CHECK_EQ(r(MC_SONIC_CR), 0x0094); T();
    /* Tally counters latch the inversion of what is written (note 4). */
    w(MC_SONIC_CRCT, 0xFFFF);
    w(MC_SONIC_MPT, 0xFFFE);
    CHECK_EQ(r(MC_SONIC_CRCT), 0); T();
    CHECK_EQ(r(MC_SONIC_MPT), 1); T();
    /* The silicon revision is read only. */
    w(MC_SONIC_SR, 0x55);
    CHECK_EQ(r(MC_SONIC_SR), MC_SONIC_REVISION); T();
}

static void test_commands_without_memory(void) {
    fresh(0);
    w(MC_SONIC_CR, 0);   /* leave reset */
    w(MC_SONIC_IMR, MC_SONIC_INT_TXDN | MC_SONIC_INT_LCD);
    CHECK_EQ(irq(), 0); T();
    /* Receiving for a long time with the link down: nothing happens. */
    w(MC_SONIC_CR, MC_SONIC_CR_RXEN);
    hw_fake_step(&fake, 100000000);
    CHECK_EQ(r(MC_SONIC_ISR), 0); T();
    CHECK_EQ(irq(), 0); T();
    /* Transmit Packets completes at once: TXP cleared, TXDN, interrupt. */
    w(MC_SONIC_CR, MC_SONIC_CR_TXP);
    CHECK_EQ(r(MC_SONIC_CR) & MC_SONIC_CR_TXP, 0); T();
    CHECK_EQ(r(MC_SONIC_ISR), MC_SONIC_INT_TXDN); T();
    CHECK_EQ(irq(), 1); T();
    w(MC_SONIC_ISR, MC_SONIC_INT_TXDN);   /* a 1 clears (4.3.6) */
    CHECK_EQ(r(MC_SONIC_ISR), 0); T();
    CHECK_EQ(irq(), 0); T();
    /* Load CAM and Read RRA complete; HTX with nothing to halt clears. */
    w(MC_SONIC_CR, MC_SONIC_CR_LCAM);
    CHECK_EQ(r(MC_SONIC_CR) & MC_SONIC_CR_LCAM, 0); T();
    CHECK_EQ(r(MC_SONIC_ISR), MC_SONIC_INT_LCD); T();
    w(MC_SONIC_CR, MC_SONIC_CR_RRRA | MC_SONIC_CR_HTX);
    CHECK_EQ(r(MC_SONIC_CR) & 0x0101, MC_SONIC_CR_HTX); T();   /* HTX reads back until a reset */
    w(MC_SONIC_CR, MC_SONIC_CR_RST);
    CHECK_EQ(r(MC_SONIC_CR) & 0x0101, 0); T();
}

static void test_cam(void) {
    fresh(1);
    /* CDA at $3000 in 16-bit mode: two descriptors (entry pointer, CAP0,
       CAP1, CAP2), then the CAM enable field (4.1.1). Address
       08:00:07:12:34:56 in CAP0 0008, CAP1 1207, CAP2 5634 (4.3.10 order). */
    poke(0x3000, 0); poke(0x3002, 0x0008); poke(0x3004, 0x1207); poke(0x3006, 0x5634);
    poke(0x3008, 5); poke(0x300A, 0x0102); poke(0x300C, 0x0304); poke(0x300E, 0x0506);
    poke(0x3010, 0x0021);
    w(MC_SONIC_URRA, 0);
    w(MC_SONIC_CDP, 0x3000);
    w(MC_SONIC_CDC, 2);
    w(MC_SONIC_CR, 0);
    w(MC_SONIC_CR, MC_SONIC_CR_LCAM);
    CHECK_EQ(r(MC_SONIC_ISR) & MC_SONIC_INT_LCD, MC_SONIC_INT_LCD); T();
    CHECK_EQ(r(MC_SONIC_CDC), 0); T();
    CHECK_EQ(r(MC_SONIC_CDP), 0x3012); T();   /* after the CAM enable field */
    CHECK_EQ(r(MC_SONIC_CE), 0x0021); T();
    /* Read back in reset mode through CEP and the CAM address ports. */
    w(MC_SONIC_CR, MC_SONIC_CR_RST);
    w(MC_SONIC_CEP, 5);
    CHECK_EQ(r(MC_SONIC_CAP0), 0x0102); T();
    CHECK_EQ(r(MC_SONIC_CAP1), 0x0304); T();
    CHECK_EQ(r(MC_SONIC_CAP2), 0x0506); T();
    w(MC_SONIC_CEP, 0);
    CHECK_EQ(r(MC_SONIC_CAP2), 0x5634); T();
    /* CE takes writes in reset mode only (note 2). */
    w(MC_SONIC_CE, 0xFFFF);
    CHECK_EQ(r(MC_SONIC_CE), 0xFFFF); T();
    w(MC_SONIC_CR, 0);
    w(MC_SONIC_CE, 0x0001);
    CHECK_EQ(r(MC_SONIC_CE), 0xFFFF); T();

    /* Read RRA: buffer pointer and word count from URRA:RRP; RRP wraps from
       REA to RSA (3.4.1). */
    poke(0x2000, 0x4000); poke(0x2002, 0x0001); poke(0x2004, 0x0300); poke(0x2006, 0);
    w(MC_SONIC_RSA, 0x2000);
    w(MC_SONIC_REA, 0x2008);
    w(MC_SONIC_RRP, 0x2000);
    w(MC_SONIC_RWP, 0x2008);
    w(MC_SONIC_CR, MC_SONIC_CR_RRRA);
    CHECK_EQ(r(MC_SONIC_CRBA0), 0x4000); T();
    CHECK_EQ(r(MC_SONIC_CRBA1), 0x0001); T();
    CHECK_EQ(r(MC_SONIC_RBWC0), 0x0300); T();
    CHECK_EQ(r(MC_SONIC_RRP), 0x2000); T();
    CHECK_EQ(r(MC_SONIC_CR) & MC_SONIC_CR_RRRA, 0); T();
}

static void test_transmit(void) {
    /* 16-bit mode, no network: two packets of one fragment, the second with
       EOL; each completes "transmitted, no carrier" (PTX, NCRS, CRSL) and,
       its source address not being in the CAM, PMB (4.3.4). */
    fresh(1);
    w(MC_SONIC_CR, 0);
    w(MC_SONIC_IMR, MC_SONIC_INT_TXDN);
    /* TXpkt at $1000: status, config, size 64, 1 fragment ($5000, 64), link $1010. */
    poke(0x1002, 0); poke(0x1004, 64); poke(0x1006, 1);
    poke(0x1008, 0x5000); poke(0x100A, 0); poke(0x100C, 64); poke(0x100E, 0x1010);
    poke(0x1012, 0); poke(0x1014, 60); poke(0x1016, 1);
    poke(0x1018, 0x5100); poke(0x101A, 0); poke(0x101C, 60); poke(0x101E, 0x1021);   /* EOL */
    w(MC_SONIC_UTDA, 0);
    w(MC_SONIC_CTDA, 0x1000);
    w(MC_SONIC_CR, MC_SONIC_CR_TXP);
    CHECK_EQ(peek(0x1000), 0x0189); T();
    CHECK_EQ(peek(0x1010), 0x0189); T();
    /* CTDA takes each link, the last one's too: the next vacant descriptor
       for a later Transmit command (3.5.3.1, 3.5.4). */
    CHECK_EQ(r(MC_SONIC_CTDA), 0x1021); T();
    CHECK_EQ(r(MC_SONIC_CR) & MC_SONIC_CR_TXP, 0); T();
    CHECK_EQ(r(MC_SONIC_ISR), MC_SONIC_INT_TXDN); T();
    CHECK_EQ(r(MC_SONIC_TCR) & 0x0FFF, 0x0189); T();
    CHECK_EQ(r(MC_SONIC_TPS), 0xFFFF - 60); T();   /* the inversion (Table 4-2, note 1) */
    CHECK_EQ(irq(), 1); T();

    /* With the network and the station's address in the CAM: three
       fragments at odd addresses gathered into one frame, PTX alone. */
    fresh(1);
    plug();
    static const uint8_t me[6] = {0x02, 0x4C, 0x56, 0x44, 0x58, 0x01};
    poke(0x3000, 0); poke(0x3002, 0x4C02); poke(0x3004, 0x4456); poke(0x3006, 0x0158); poke(0x3008, 1);
    w(MC_SONIC_CDP, 0x3000);
    w(MC_SONIC_CDC, 1);
    w(MC_SONIC_CR, 0);
    w(MC_SONIC_CR, MC_SONIC_CR_LCAM);
    for (unsigned k = 0; k < 64; k++) mem[0x5001 + k] = (uint8_t)k;
    memcpy(mem + 0x5001 + 6, me, 6);
    poke(0x1002, 0); poke(0x1004, 64); poke(0x1006, 3);
    poke(0x1008, 0x5001); poke(0x100A, 0); poke(0x100C, 13);
    poke(0x100E, 0x500E); poke(0x1010, 0); poke(0x1012, 50);
    poke(0x1014, 0x5040); poke(0x1016, 0); poke(0x1018, 1);
    poke(0x101A, 0x1021);
    w(MC_SONIC_CTDA, 0x1000);
    w(MC_SONIC_CR, MC_SONIC_CR_TXP);
    CHECK_EQ(peek(0x1000), MC_SONIC_TCR_PTX); T();
    CHECK_EQ(sent_count, 1); T();
    CHECK_EQ(sent_length[0], 64); T();
    CHECK(!memcmp(sent[0], mem + 0x5001, 64)); T();
    CHECK_EQ(r(MC_SONIC_TFC), 3); T();

    /* 32-bit mode: each field in the low half of a long word; a size that
       does not match the fragments aborts with BCM and TXER, CTDA stays on
       the packet (4.3.4, 3.5.3.2). */
    fresh(1);
    plug();
    w(MC_SONIC_DCR, MC_SONIC_DCR_DW);
    w(MC_SONIC_CR, 0);
    /* TXpkt at $1004: fields at $1006 + 4n. */
    poke(0x100A, 0); poke(0x100E, 100); poke(0x1012, 1);
    poke(0x1016, 0x6000); poke(0x101A, 0); poke(0x101E, 99); poke(0x1022, 0x0001);
    w(MC_SONIC_CTDA, 0x1004);
    w(MC_SONIC_CR, MC_SONIC_CR_TXP);
    CHECK_EQ(peek(0x1006), MC_SONIC_TCR_BCM); T();
    CHECK_EQ(r(MC_SONIC_ISR), MC_SONIC_INT_TXDN | MC_SONIC_INT_TXER); T();
    CHECK_EQ(r(MC_SONIC_CTDA), 0x1004); T();
    CHECK_EQ(sent_count, 0); T();
    CHECK_EQ(irq(), 0); T();   /* IMR clear */
    /* The programmable interrupt bit of TXpkt.config sets PINT. */
    fresh(1);
    w(MC_SONIC_CR, 0);
    poke(0x1002, 0x8000); poke(0x1004, 10); poke(0x1006, 1); poke(0x100C, 10); poke(0x100E, 1);
    w(MC_SONIC_CTDA, 0x1000);
    w(MC_SONIC_CR, MC_SONIC_CR_TXP);
    CHECK_EQ(r(MC_SONIC_ISR) & MC_SONIC_INT_PINT, MC_SONIC_INT_PINT); T();
}

/* Receive set-up in 32-bit mode (3.4.4): the CAM holds the station; an RRA
 * at $2000 of `buffers` descriptors (RBAs of `words` words at $8000, $9000,
 * ...), RWP after them; an RDA at $4000 of `descs` descriptors of 7 long
 * words, the last with EOL; EOBC 760 words. */
static const uint8_t station[6] = {0x02, 0x4C, 0x56, 0x44, 0x58, 0x01};
static void rx_setup(unsigned buffers, uint16_t words, unsigned descs) {
    fresh(1);
    plug();
    w(MC_SONIC_DCR, MC_SONIC_DCR_DW);
    w(MC_SONIC_CR, 0);
    /* CAM entry 0, enabled (4.1.1, 32-bit fields). */
    poke(0x3002, 0); poke(0x3006, 0x4C02); poke(0x300A, 0x4456); poke(0x300E, 0x0158); poke(0x3012, 1);
    w(MC_SONIC_URRA, 0);
    w(MC_SONIC_CDP, 0x3000);
    w(MC_SONIC_CDC, 1);
    w(MC_SONIC_CR, MC_SONIC_CR_LCAM);
    for (unsigned b = 0; b < buffers; b++) {
        const uint32_t d = 0x2000 + 16 * b;
        poke(d + 2, (uint16_t)(0x8000 + 0x1000 * b)); poke(d + 6, 0); poke(d + 10, words); poke(d + 14, 0);
    }
    w(MC_SONIC_RSA, 0x2000);
    w(MC_SONIC_REA, (uint16_t)(0x2000 + 16 * 8));
    w(MC_SONIC_RRP, 0x2000);
    w(MC_SONIC_RWP, (uint16_t)(0x2000 + 16 * buffers));
    w(MC_SONIC_CR, MC_SONIC_CR_RRRA);
    for (unsigned k = 0; k < descs; k++) {
        const uint32_t d = 0x4000 + 28 * k;
        poke(d + 22, k + 1 == descs ? 1 : (uint16_t)(d + 28));   /* link */
        poke(d + 26, 0xFFFF);                                    /* in_use */
    }
    w(MC_SONIC_URDA, 0);
    w(MC_SONIC_CRDA, 0x4000);
    w(MC_SONIC_EOBC, 760);
    w(MC_SONIC_RCR, 0);
    w(MC_SONIC_IMR, MC_SONIC_INT_PKTRX | MC_SONIC_INT_RDE | MC_SONIC_INT_RBE);
    w(MC_SONIC_ISR, 0xFFFF);
    w(MC_SONIC_CR, MC_SONIC_CR_RXEN);
}
/* A frame of `length` bytes (no FCS) to `dst`, from 02:00:00:00:00:99. */
static void frame(uint8_t *f, const uint8_t *dst, size_t length, uint8_t seed) {
    memcpy(f, dst, 6);
    static const uint8_t src[6] = {0x02, 0, 0, 0, 0, 0x99};
    memcpy(f + 6, src, 6);
    for (size_t k = 12; k < length; k++) f[k] = (uint8_t)(seed + k);
}
/* The time a frame takes on the wire, in C15M periods (rounded up). */
static uint64_t on_wire(size_t length) { return ((length + 4) * 8u + 160u) * 15667200ull / 10000000u + 1u; }

static void test_receive(void) {
    rx_setup(2, 1600, 3);
    CHECK_EQ(r(MC_SONIC_CRBA0), 0x8000); T();
    CHECK_EQ(r(MC_SONIC_RBWC0), 1600); T();
    CHECK_EQ(r(MC_SONIC_RRP), 0x2010); T();
    uint8_t f[1514];
    frame(f, station, 100, 1);
    CHECK_EQ(mc_sonic_receive(&sonic, f, 100), 1); T();
    /* Nothing before the frame's last bit is in. */
    hw_fake_step(&fake, on_wire(100) - 3);
    CHECK_EQ(r(MC_SONIC_ISR), 0); T();
    hw_fake_step(&fake, 3);
    CHECK_EQ(r(MC_SONIC_ISR), MC_SONIC_INT_PKTRX); T();
    CHECK_EQ(irq(), 1); T();
    /* The frame with its FCS in the RBA, the last long word filled with
       FFh (1.4.1); the descriptor: status PRX, byte count with the FCS,
       the packet pointer, sequence numbers, in_use cleared (3.4.6.1). */
    CHECK(!memcmp(mem + 0x8000, f, 100)); T();
    const uint32_t crc = mc_sonic_crc32(f, 100);
    CHECK_EQ(mem[0x8064], (uint8_t)crc); T();
    CHECK_EQ(mem[0x8067], (uint8_t)(crc >> 24)); T();
    CHECK_EQ(peek(0x4002), MC_SONIC_RCR_PRX | MC_SONIC_RCR_CRS); T();
    CHECK_EQ(peek(0x4006), 104); T();
    CHECK_EQ(peek(0x400A), 0x8000); T();
    CHECK_EQ(peek(0x400E), 0); T();
    CHECK_EQ(peek(0x4012), 0x0000); T();
    CHECK_EQ(peek(0x401A), 0); T();
    CHECK_EQ(r(MC_SONIC_CRDA), 0x401C); T();
    CHECK_EQ(r(MC_SONIC_CRBA0), 0x8068); T();
    CHECK_EQ(r(MC_SONIC_RBWC0), 1600 - 52); T();
    CHECK_EQ(r(MC_SONIC_RSC), 0x0001); T();
    /* A frame of 61 bytes plus its FCS: 65 bytes, three FFh fill bytes
       after it to the long word. */
    w(MC_SONIC_ISR, 0xFFFF);
    CHECK_EQ(irq(), 0); T();
    frame(f, station, 61, 7);
    mc_sonic_receive(&sonic, f, 61);
    hw_fake_step(&fake, on_wire(61));
    CHECK_EQ(peek(0x4006 + 28), 65); T();
    CHECK_EQ(peek(0x400A + 28), 0x8068); T();
    CHECK_EQ(mem[0x8068 + 65], 0xFF); T();
    CHECK_EQ(mem[0x8068 + 67], 0xFF); T();
    CHECK_EQ(peek(0x4012 + 28), 0x0001); T();
    /* Filtering: another station's frame, a broadcast and a multicast are
       dropped until BRD and AMC are set (4.3.3). */
    static const uint8_t other[6] = {0x02, 0, 0, 0, 0, 0x42}, bcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF},
                         mcast[6] = {0x09, 0, 0x07, 0xFF, 0xFF, 0xFF};
    w(MC_SONIC_ISR, 0xFFFF);
    frame(f, other, 80, 0); mc_sonic_receive(&sonic, f, 80);
    frame(f, bcast, 80, 0); mc_sonic_receive(&sonic, f, 80);
    frame(f, mcast, 80, 0); mc_sonic_receive(&sonic, f, 80);
    hw_fake_step(&fake, 3 * on_wire(80));
    CHECK_EQ(r(MC_SONIC_ISR), 0); T();
    CHECK_EQ(r(MC_SONIC_CRDA), 0x4038); T();
    w(MC_SONIC_RCR, MC_SONIC_RCR_BRD);
    frame(f, bcast, 80, 0); mc_sonic_receive(&sonic, f, 80);
    hw_fake_step(&fake, on_wire(80));
    /* The third and last descriptor: BC in its status; its link has EOL,
       so RDE, it keeps in_use and CRDA holds its link field (3.4.6.1). */
    CHECK_EQ(peek(0x4002 + 56), MC_SONIC_RCR_BRD | MC_SONIC_RCR_BC | MC_SONIC_RCR_PRX | MC_SONIC_RCR_CRS); T();
    CHECK_EQ(r(MC_SONIC_ISR), MC_SONIC_INT_PKTRX | MC_SONIC_INT_RDE); T();
    CHECK_EQ(peek(0x401A + 56), 0xFFFF); T();
    CHECK_EQ(r(MC_SONIC_CRDA), 0x4038 + 20); T();
    /* No descriptor: the next frame is missed (MPT, 4.3.11). */
    w(MC_SONIC_ISR, 0xFFFF);
    w(MC_SONIC_MPT, 0xFFFF);
    frame(f, station, 80, 0); mc_sonic_receive(&sonic, f, 80);
    hw_fake_step(&fake, on_wire(80));
    CHECK_EQ(r(MC_SONIC_ISR), 0); T();
    CHECK_EQ(r(MC_SONIC_MPT), 1); T();
    /* The system appends a descriptor at $4054 and clears EOL: the next
       frame frees the kept descriptor and goes into the new one (3.4.7). */
    poke(0x4054 + 22, 1); poke(0x4054 + 26, 0xFFFF);
    poke(0x4038 + 22, 0x4054);
    w(MC_SONIC_RCR, MC_SONIC_RCR_AMC);
    frame(f, mcast, 90, 3); mc_sonic_receive(&sonic, f, 90);
    hw_fake_step(&fake, on_wire(90));
    CHECK_EQ(peek(0x401A + 56), 0); T();
    CHECK_EQ(peek(0x4006 + 84), 94); T();
    CHECK_EQ(peek(0x4002 + 84) & 0x01FF, MC_SONIC_RCR_MC | MC_SONIC_RCR_PRX | MC_SONIC_RCR_CRS); T();
    /* The receiver disabled: missed (4.3.11). */
    w(MC_SONIC_CR, MC_SONIC_CR_RXDIS);
    frame(f, station, 80, 0); mc_sonic_receive(&sonic, f, 80);
    hw_fake_step(&fake, on_wire(80));
    CHECK_EQ(r(MC_SONIC_MPT), 2); T();
}

static void test_receive_buffers(void) {
    /* Two RBAs of 800 words with EOBC 760: each takes one large packet
       (LPKT), then the next RBA is read with its sequence number stepped
       (3.4.2.2, 4.3.9); reading the last descriptor sets RBE (3.4.7). */
    rx_setup(2, 800, 4);
    uint8_t f[1514];
    CHECK_EQ(r(MC_SONIC_ISR), 0); T();
    frame(f, station, 1000, 0);
    mc_sonic_receive(&sonic, f, 1000);
    hw_fake_step(&fake, on_wire(1000));
    CHECK_EQ(peek(0x4002) & MC_SONIC_RCR_LPKT, MC_SONIC_RCR_LPKT); T();
    CHECK_EQ(r(MC_SONIC_CRBA0), 0x9000); T();
    CHECK_EQ(r(MC_SONIC_RSC), 0x0100); T();
    CHECK_EQ(r(MC_SONIC_ISR) & MC_SONIC_INT_RBE, MC_SONIC_INT_RBE); T();
    /* The last RBA used up with RBE set: no buffer; the frame after it is
       missed. */
    mc_sonic_receive(&sonic, f, 1000);
    hw_fake_step(&fake, on_wire(1000));
    CHECK_EQ(peek(0x400A + 28), 0x9000); T();
    CHECK_EQ(peek(0x4012 + 28), 0x0100); T();
    w(MC_SONIC_MPT, 0xFFFF);
    mc_sonic_receive(&sonic, f, 1000);
    hw_fake_step(&fake, on_wire(1000));
    CHECK_EQ(r(MC_SONIC_MPT), 1); T();
    /* The system adds a resource and clears RBE: the SONIC reads it (ISR
       note 3) and receives again. */
    poke(0x2020 + 2, 0xA000); poke(0x2020 + 10, 800);
    w(MC_SONIC_RWP, 0x2030);
    w(MC_SONIC_ISR, MC_SONIC_INT_RBE);
    CHECK_EQ(r(MC_SONIC_CRBA0), 0xA000); T();
    mc_sonic_receive(&sonic, f, 1000);
    hw_fake_step(&fake, on_wire(1000));
    CHECK_EQ(peek(0x400A + 56), 0xA000); T();
    CHECK_EQ(peek(0x4012 + 56), 0x0200); T();
    /* A frame larger than what is left in the RBA (EOBC too small): RBAE,
       no descriptor, the next RBA (3.4.7). */
    rx_setup(3, 300, 4);
    w(MC_SONIC_EOBC, 100);
    frame(f, station, 700, 0);
    mc_sonic_receive(&sonic, f, 700);
    hw_fake_step(&fake, on_wire(700));
    CHECK_EQ(r(MC_SONIC_ISR) & (MC_SONIC_INT_RBAE | MC_SONIC_INT_PKTRX), MC_SONIC_INT_RBAE); T();
    CHECK_EQ(r(MC_SONIC_CRBA0), 0x9000); T();
    CHECK_EQ(r(MC_SONIC_CRDA), 0x4000); T();
    /* The wire holds MC_SONIC_WIRE frames; one more waits for room. */
    rx_setup(2, 1600, 3);
    for (unsigned k = 0; k < MC_SONIC_WIRE; k++) CHECK_EQ(mc_sonic_receive(&sonic, f, 60), 1);
    CHECK_EQ(mc_sonic_receive(&sonic, f, 60), 0); T();
    hw_fake_step(&fake, on_wire(60));
    CHECK_EQ(mc_sonic_receive(&sonic, f, 60), 1); T();
}

static void test_appletalk_multicast(void) {
    /* AppleTalk's multicast 09:00:07:FF:FF:FF in a CAM entry the system
       loads: accepted without AMC, with MC in its status (1.2.1, 4.3.3);
       another multicast is not. */
    rx_setup(2, 1600, 3);
    poke(0x3102, 1); poke(0x3106, 0x0009); poke(0x310A, 0xFF07); poke(0x310E, 0xFFFF); poke(0x3112, 0x0003);
    w(MC_SONIC_CDP, 0x3100);
    w(MC_SONIC_CDC, 1);
    w(MC_SONIC_CR, MC_SONIC_CR_LCAM);
    CHECK_EQ(r(MC_SONIC_CE), 3); T();
    static const uint8_t atalk[6] = {0x09, 0x00, 0x07, 0xFF, 0xFF, 0xFF}, zone[6] = {0x09, 0x00, 0x07, 0x00, 0x00, 0x2A};
    uint8_t f[100];
    frame(f, zone, 100, 0);
    mc_sonic_receive(&sonic, f, 100);
    hw_fake_step(&fake, on_wire(100));
    CHECK_EQ(r(MC_SONIC_CRDA), 0x4000); T();
    frame(f, atalk, 100, 0);
    mc_sonic_receive(&sonic, f, 100);
    hw_fake_step(&fake, on_wire(100));
    CHECK_EQ(r(MC_SONIC_CRDA), 0x401C); T();
    CHECK_EQ(peek(0x4002) & 0x01FF, MC_SONIC_RCR_MC | MC_SONIC_RCR_PRX | MC_SONIC_RCR_CRS); T();
}

static void test_loopback(void) {
    /* MAC loopback (LB1-LB0 = 01, 1.7): a transmitted packet to the
       station comes back with LBK and never reaches the network; NCRS and
       CRSL are always set (4.3.4). */
    rx_setup(2, 1600, 3);
    w(MC_SONIC_RCR, 0x0200);
    for (unsigned k = 0; k < 64; k++) mem[0x5000 + k] = (uint8_t)(k * 3);
    memcpy(mem + 0x5000, station, 6);
    memcpy(mem + 0x5006, station, 6);
    poke(0x100A, 0); poke(0x100E, 64); poke(0x1012, 1);
    poke(0x1016, 0x5000); poke(0x101A, 0); poke(0x101E, 64); poke(0x1022, 1);
    w(MC_SONIC_CTDA, 0x1004);
    w(MC_SONIC_CR, MC_SONIC_CR_TXP);
    CHECK_EQ(peek(0x1006), MC_SONIC_TCR_PTX | MC_SONIC_TCR_NCRS | MC_SONIC_TCR_CRSL); T();
    CHECK_EQ(sent_count, 0); T();
    hw_fake_step(&fake, on_wire(64));
    CHECK_EQ(peek(0x4002) & 0x01FF, MC_SONIC_RCR_LBK | MC_SONIC_RCR_PRX | MC_SONIC_RCR_CRS); T();
    CHECK(!memcmp(mem + 0x8000, mem + 0x5000, 64)); T();
    /* Frames from the network do not reach a SONIC in MAC loopback. */
    uint8_t f[100];
    frame(f, station, 100, 0);
    mc_sonic_receive(&sonic, f, 100);
    hw_fake_step(&fake, on_wire(100));
    CHECK_EQ(r(MC_SONIC_CRDA), 0x401C); T();
    /* RJCM rejects the CAM's own address; PRO then takes every physical
       address (4.3.7, 4.3.3). */
    rx_setup(2, 1600, 3);
    w(MC_SONIC_CR, MC_SONIC_CR_RST);
    w(MC_SONIC_DCR2, MC_SONIC_DCR2_RJCM);
    w(MC_SONIC_CR, 0);
    w(MC_SONIC_CR, MC_SONIC_CR_RXEN);
    mc_sonic_receive(&sonic, f, 100);
    hw_fake_step(&fake, on_wire(100));
    CHECK_EQ(r(MC_SONIC_CRDA), 0x4000); T();
    w(MC_SONIC_RCR, MC_SONIC_RCR_PRO);
    mc_sonic_receive(&sonic, f, 100);
    hw_fake_step(&fake, on_wire(100));
    CHECK_EQ(r(MC_SONIC_CRDA), 0x401C); T();
}

static void test_timer(void) {
    fresh(0);
    w(MC_SONIC_CR, 0);
    w(MC_SONIC_IMR, MC_SONIC_INT_TC);
    /* 99 counts down to 0 and rolls over on the 100th tick at 5 MHz:
       100 x 15.6672 / 5 = 313.3 C15M periods (4.3.12). */
    w(MC_SONIC_WT0, 99);
    w(MC_SONIC_WT1, 0);
    w(MC_SONIC_CR, MC_SONIC_CR_ST);
    CHECK_EQ(r(MC_SONIC_CR) & 0x30, MC_SONIC_CR_ST); T();
    hw_fake_step(&fake, 160);   /* 51 ticks */
    CHECK_EQ(r(MC_SONIC_WT0), 99 - 51); T();
    hw_fake_step(&fake, 153);   /* 313 periods: 99.9 ticks */
    CHECK_EQ(r(MC_SONIC_ISR), 0); T();
    hw_fake_step(&fake, 1);
    CHECK_EQ(r(MC_SONIC_ISR), MC_SONIC_INT_TC); T();
    CHECK_EQ(irq(), 1); T();
    CHECK_EQ(r(MC_SONIC_WT1), 0xFFFF); T();   /* counting on from FFFFFFFFh */
    /* STP halts it; the value holds. */
    w(MC_SONIC_CR, MC_SONIC_CR_STP);
    uint16_t held = r(MC_SONIC_WT0);
    hw_fake_step(&fake, 10000);
    CHECK_EQ(r(MC_SONIC_WT0), held); T();
    CHECK_EQ(r(MC_SONIC_CR) & 0x30, MC_SONIC_CR_STP); T();
    CHECK_EQ(hw_fake_deadline(&fake, &sonic), MC_NEVER); T();
}

static void test_prom(void) {
    mc_sonic_prom p;
    hw_fake_init(&fake);
    mc_sonic_prom_init(&p, &fake.host, NULL, 2, 0);
    CHECK_EQ(p.dev.ops->read(&p, 0, 1), 0x08); T();   /* Apple's block, unicast */
    CHECK_EQ(p.dev.ops->read(&p, 8, 1), 0x07); T();
    CHECK_EQ(p.dev.ops->read(&p, 20, 1), 0x43); T();
    CHECK_EQ(p.dev.ops->read(&p, 32, 1), 0x00); T();
    p.dev.ops->write(&p, 0, 1, 0xFF);
    CHECK_EQ(p.dev.ops->read(&p, 0, 1), 0x08); T();
    /* As the Quadra 700's: consecutive bytes, bits reversed, eight bytes
       XOR to FFh. */
    static const uint8_t a[6] = {0x02, 0x80, 0x01, 0x00, 0x00, 0xC0};
    mc_sonic_prom_init(&p, &fake.host, a, 0, 1);
    CHECK_EQ(p.dev.ops->read(&p, 0, 1), 0x40); T();
    CHECK_EQ(p.dev.ops->read(&p, 1, 1), 0x01); T();
    CHECK_EQ(p.dev.ops->read(&p, 2, 1), 0x80); T();
    CHECK_EQ(p.dev.ops->read(&p, 5, 1), 0x03); T();
    uint8_t x = 0;
    for (unsigned k = 0; k < 8; k++) x ^= (uint8_t)p.dev.ops->read(&p, k, 1);
    CHECK_EQ(x, 0xFF); T();
    CHECK_EQ(p.dev.ops->read(&p, 0, 4), 0x40018000u); T();
}

int main(void) {
    test_reset();
    test_commands_without_memory();
    test_cam();
    test_transmit();
    test_receive();
    test_receive_buffers();
    test_loopback();
    test_appletalk_multicast();
    test_timer();
    test_prom();
    printf("test_sonic: %d tests, %d failure(s)\n", tests, mc_test_failures);
    return mc_test_failures ? 1 : 0;
}
