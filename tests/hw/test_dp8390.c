/* test_dp8390: the DP8390 NIC (dp8390.h) as the EtherTalk NB Card's driver
 * drives it: reset state (CR 21h, ISR RST), the initialisation sequence
 * (DCR, RCR, TCR, the ring, ISR, IMR, page 1's PAR and CURR), reception into
 * the ring with the four-byte header and the FCS after the frame's wire
 * time, the address filter (PAR, broadcast with AB, multicast through MAR
 * with AM), the INT line through ISR and IMR, transmission from TPSR with
 * TXP and PTX, ring overflow (OVW, the missed packet counter cleared when
 * read) and the remote DMA port. */
#include "hw_test.h"
#include "dp8390.h"

#include <string.h>

static hw_fake fake;
static mc_dp8390 nic;
static uint8_t mem[0x4000];
static uint8_t sent[1600];
static size_t sent_len;
static unsigned sent_count;

static void wr(unsigned r, uint8_t v) { mc_dp8390_write(&nic, r, v); }
static uint8_t rd(unsigned r) { return mc_dp8390_read(&nic, r); }
static void on_transmit(void *ctx, const uint8_t *f, size_t n) {
    (void)ctx;
    memcpy(sent, f, n);
    sent_len = n;
    sent_count++;
}
static void settle(void) { hw_fake_step(&fake, 2000000); }

int main(void) {
    static const uint8_t me[6] = {0x02, 0x60, 0x8C, 0x05, 0xAC, 0x62};
    hw_fake_init(&fake);
    mc_dp8390_init(&nic, &fake.host, mem, sizeof mem);
    const mc_dp8390_port port = {NULL, on_transmit};
    mc_dp8390_set_port(&nic, &port);
    CHECK_EQ(rd(0), 0x21);
    CHECK_EQ(rd(7) & 0x80u, 0x80u);
    /* The NB driver's initialisation. */
    wr(0, 0x21);
    wr(14, 0x49);
    wr(12, 0x0C);
    wr(13, 0x02);
    wr(1, 0x03);
    wr(2, 0x40);
    wr(3, 0x3F);
    wr(7, 0xFF);
    wr(15, 0x1F);
    wr(0, 0x61);
    for (unsigned i = 0; i < 6; i++) wr(1 + i, me[i]);
    wr(7, 0x03);
    for (unsigned i = 0; i < 8; i++) wr(8 + i, 0);
    CHECK_EQ(rd(7), 0x03);                              /* page 1: CURR */
    wr(0, 0x22);
    wr(13, 0x00);
    CHECK_EQ(rd(7) & 0x80u, 0u);                        /* started: RST cleared */
    /* A frame to the station: header, data and FCS at page 3. */
    uint8_t f[100];
    memset(f, 0x5A, sizeof f);
    memcpy(f, me, 6);
    CHECK_EQ(mc_dp8390_receive(&nic, f, 60), 1);
    CHECK_EQ(rd(7) & 1u, 0u);                           /* not before its wire time */
    settle();
    CHECK_EQ(rd(7) & 1u, 1u);
    CHECK_EQ(hw_fake_line(&fake, &nic, MC_DP8390_LINE_IRQ), 1);
    CHECK_EQ(mem[0x300], 0x01);                         /* RSR PRX */
    CHECK_EQ(mem[0x301], 0x04);                         /* next page */
    CHECK_EQ(mem[0x302], 64u);                          /* count low, with the FCS */
    CHECK_EQ(mem[0x303], 0u);
    CHECK(!memcmp(mem + 0x304, f, 60));
    wr(7, 0x01);
    CHECK_EQ(hw_fake_line(&fake, &nic, MC_DP8390_LINE_IRQ), 0);
    wr(0, 0x62);
    CHECK_EQ(rd(7), 0x04);                              /* CURR moved */
    wr(0, 0x22);
    /* The filter: another station dropped, a broadcast taken (PHY). */
    f[0] = 0x08;
    mc_dp8390_receive(&nic, f, 60);
    settle();
    CHECK_EQ(rd(7) & 1u, 0u);
    memset(f, 0xFF, 6);
    mc_dp8390_receive(&nic, f, 60);
    settle();
    CHECK_EQ(rd(7) & 1u, 1u);
    CHECK_EQ(mem[0x400], 0x21);
    wr(7, 0x01);
    /* AppleTalk's multicast: only with its hash bit in MAR. */
    static const uint8_t at[6] = {0x09, 0x00, 0x07, 0xFF, 0xFF, 0xFF};
    memcpy(f, at, 6);
    mc_dp8390_receive(&nic, f, 60);
    settle();
    CHECK_EQ(rd(7) & 1u, 0u);
    const unsigned h = mc_dp8390_hash(at);
    wr(0, 0x62);
    wr(8 + (h >> 3), (uint8_t)(1u << (h & 7)));
    wr(0, 0x22);
    mc_dp8390_receive(&nic, f, 60);
    settle();
    CHECK_EQ(rd(7) & 1u, 1u);
    wr(7, 0x01);
    /* Transmission from page 0. */
    for (unsigned i = 0; i < 60; i++) mem[i] = (uint8_t)i;
    wr(4, 0x00);
    wr(5, 60);
    wr(6, 0);
    wr(0, 0x26);
    CHECK_EQ(sent_count, 1u);
    CHECK_EQ(sent_len, 60u);
    CHECK_EQ(sent[59], 59u);
    CHECK_EQ(rd(0) & 0x04u, 0x04u);                     /* TXP until done */
    settle();
    CHECK_EQ(rd(0) & 0x04u, 0u);
    CHECK_EQ(rd(7) & 2u, 2u);
    CHECK_EQ(rd(4) & 1u, 1u);                           /* TSR PTX */
    wr(7, 0x02);
    /* Overflow: BNRY just ahead of CURR leaves no room. */
    wr(0, 0x62);
    const uint8_t curr = rd(7);
    wr(0, 0x22);
    wr(3, curr);
    memcpy(f, me, 6);
    mc_dp8390_receive(&nic, f, 60);
    settle();
    CHECK_EQ(rd(7) & 0x11u, 0x10u);                     /* OVW, no PRX */
    CHECK_EQ(rd(15), 1u);                               /* CNTR2 */
    CHECK_EQ(rd(15), 0u);                               /* cleared when read */
    /* Remote DMA read of the first packet's header. */
    wr(8, 0x00);
    wr(9, 0x03);
    wr(10, 4);
    wr(11, 0);
    wr(0, 0x0A);
    CHECK_EQ(rd(16), 0x01);
    CHECK_EQ(rd(16), 0x04);
    rd(16);
    rd(16);
    CHECK_EQ(rd(7) & 0x40u, 0x40u);                     /* RDC */
    printf("test_dp8390: %d failure(s)\n", mc_test_failures);
    return mc_test_failures ? 1 : 0;
}
