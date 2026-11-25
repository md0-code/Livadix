/* Egret at the protocol level: the shared checks (adbctl_suite.c) and
 * Egret's own pseudo-commands, as 341S0851 answers them (egret.c). */
#include "adbctl_suite.h"

static void egret_commands(ac_host *h) {
    CHECK(EXPECT(h, 0, P(1, 0x04), P(1, 0, 0x04, 0x10, 0x00)));
    CHECK(EXPECT(h, 0, P(1, 0x05), P(1, 0, 0x05, 0x0F, 0x00)));
    CHECK(EXPECT(h, 0, P(1, 0x06), P(1, 0, 0x06, 0x0F, 0x5A)));
    CHECK(EXPECT(h, 0, P(1, 0x15), P(1, 0, 0x15, 0x01, 0x08)));
    CHECK(EXPECT(h, 0, P(1, 0x0F), P(1, 0, 0x0F, 0x00, 0x00)));         /* self-test passed */
    CHECK(EXPECT(h, 0, P(1, 0x17, 0x20), P(1, 0, 0x17)));
    CHECK(EXPECT(h, 0, P(1, 0x18), P(1, 0, 0x18, 0x20)));
    CHECK(EXPECT(h, 0, P(1, 0x1E, 0x00), P(1, 0, 0x1E)));
    CHECK(EXPECT(h, 0, P(1, 0x1F), P(1, 0, 0x1F, 0xFF)));               /* 0 stands for $FF */
    CHECK(EXPECT(h, 0, P(1, 0x1D, 0x01), P(1, 0, 0x1D)));
    CHECK(h->ctl.mem[0xA1] & 0x40);
    CHECK(EXPECT(h, 0, P(1, 0x0E, 1, 2, 3, 4), P(1, 0, 0x0E)));
    CHECK(EXPECT(h, 0, P(1, 0x0E, 1, 2, 3, 4, 5), P(2, 3, 1, 0x0E)));   /* one to four bytes */
    /* The model strap (port A bit 5) gates some commands. */
    CHECK(EXPECT(h, 0, P(1, 0x10), P(2, 2, 1, 0x10)));
    CHECK(EXPECT(h, 0, P(1, 0x12, 0x01), P(2, 2, 1, 0x12)));
    h->ctl.strap = 0;
    CHECK(EXPECT(h, 0, P(1, 0x10), P(1, 0, 0x10, 0xFF)));
    CHECK(EXPECT(h, 0, P(1, 0x12, 0x01), P(1, 0, 0x12)));
    CHECK(EXPECT(h, 0, P(1, 0x13, 0x01), P(2, 2, 1, 0x13)));
    CHECK(EXPECT(h, 0, P(1, 0x0B, 0, 0, 0, 0), P(2, 2, 1, 0x0B)));
    CHECK(EXPECT(h, 0, P(1, 0x0A), P(1, 0, 0x0A)));                     /* no power off: a reply */
    CHECK(h->ctl.line_state[MC_ADBCTL_LINE_POWER]);
    h->ctl.strap = 1;
    CHECK(EXPECT(h, 0, P(1, 0x20), P(2, 2, 1, 0x20)));
}

int main(void) {
    adbctl_suite(MC_ADBCTL_EGRET);
    egret_commands(adbctl_suite_host());
    printf("test_egret: %d failure(s)\n", mc_test_failures);
    return mc_test_failures ? 1 : 0;
}
