/* test_am79c940: the MACE (am79c940.h) as the AV boards' Ethernet driver
 * drives it: CHIPID, the software reset, the station address through IAC
 * and PADR, a frame sent in internal loopback coming back to the receiver
 * with its FCS and status bytes, XMTINT through IR and IMR (IR cleared by
 * reading it), XMTFS valid, the address filter on a frame from the line. */
#include "hw_test.h"
#include "am79c940.h"

#include <string.h>

static hw_fake fake;
static mc_am79c940 m;
static uint8_t got[1600], got_status[4];
static size_t got_len;
static unsigned sent;

static void wr(unsigned r, uint8_t v) { mc_am79c940_ops.write(&m, r << 4, 1, v); }
static uint8_t rd(unsigned r) { return (uint8_t)mc_am79c940_ops.read(&m, r << 4, 1); }
static void transmit(void *ctx, const uint8_t *f, size_t n) { (void)ctx; (void)f; (void)n; sent++; }
static int receive(void *ctx, const uint8_t *f, size_t n, const uint8_t st[4]) {
    (void)ctx;
    memcpy(got, f, n);
    got_len = n;
    memcpy(got_status, st, 4);
    return 1;
}

int main(void) {
    static const uint8_t me[6] = {0x08, 0x00, 0x07, 0x4D, 0x41, 0x43};
    hw_fake_init(&fake);
    mc_am79c940_init(&m, &fake.host, "MACE");
    const mc_am79c940_port port = {NULL, transmit, receive};
    mc_am79c940_set_port(&m, &port);
    wr(11, 0x01);                                   /* BIUCC SWRST */
    CHECK_EQ(rd(11) & 1u, 0u);
    CHECK_EQ(rd(16), 0x40);                         /* CHIPID $0940 */
    CHECK_EQ(rd(17), 0x09);
    wr(18, 0x80);                                   /* IAC: ADDRCHG, done at once */
    CHECK_EQ(rd(18) & 0x80u, 0u);
    wr(18, 0x04);                                   /* PHYADDR */
    for (unsigned i = 0; i < 6; i++) wr(21, me[i]);
    wr(9, 0x02);                                    /* IMR: all but RCVINT */
    wr(13, 0x03);                                   /* MACCC: ENXMT, ENRCV */
    wr(29, 0x04);                                   /* UTR: internal loopback */
    uint8_t f[60] = {0};
    memcpy(f, me, 6);
    memcpy(f + 6, me, 6);
    f[12] = 0x08;
    mc_am79c940_transmit(&m, f, 60);
    CHECK_EQ(sent, 0u);
    CHECK_EQ(got_len, 64u);                          /* with the FCS */
    CHECK_EQ(got_status[0], 64u);
    CHECK_EQ(got_status[1], 0u);
    CHECK(!memcmp(got, f, 60));
    CHECK_EQ(hw_fake_line(&fake, &m, MC_AM79C940_LINE_IRQ), 1);
    CHECK_EQ(rd(8), 0x01);                          /* IR: XMTINT, cleared by the read */
    CHECK_EQ(hw_fake_line(&fake, &m, MC_AM79C940_LINE_IRQ), 0);
    CHECK_EQ(hw_fake_line(&fake, &m, MC_AM79C940_LINE_TX_STATUS), 1);
    CHECK_EQ(rd(3), 0x80);                          /* XMTFS valid */
    CHECK_EQ(hw_fake_line(&fake, &m, MC_AM79C940_LINE_TX_STATUS), 0);
    wr(29, 0x00);
    mc_am79c940_transmit(&m, f, 60);
    CHECK_EQ(sent, 1u);
    /* From the line: another station's address is filtered out, a broadcast taken. */
    got_len = 0;
    f[0] = 0x02;
    CHECK_EQ(mc_am79c940_receive(&m, f, 60), 1);
    CHECK_EQ(got_len, 0u);
    memset(f, 0xFF, 6);
    CHECK_EQ(mc_am79c940_receive(&m, f, 60), 1);
    CHECK_EQ(got_len, 64u);
    printf("test_am79c940: %d failure(s)\n", mc_test_failures);
    return mc_test_failures ? 1 : 0;
}
