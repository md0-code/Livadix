/* Cuda at the protocol level: the shared checks (adbctl_suite.c) and
 * Cuda's own pseudo-commands, as 341S0788 answers them (cuda.c). */
#include "adbctl_suite.h"
#include "ppc/chips/sdram_spd.h"

static void cuda_commands(ac_host *h) {
    /* Commands Egret has and Cuda refuses. */
    CHECK(EXPECT(h, 0, P(1, 0x04), P(2, 2, 1, 0x04)));
    CHECK(EXPECT(h, 0, P(1, 0x0F), P(2, 2, 1, 0x0F)));
    CHECK(EXPECT(h, 0, P(1, 0x17, 0x01), P(2, 2, 1, 0x17)));
    /* Was the clock set? $FF until pseudo-command $09. */
    static ac_host g;
    CHECK(ac_init(&g, MC_ADBCTL_CUDA, NULL));
    CHECK(EXPECT(&g, 0, P(1, 0x10), P(1, 0, 0x10, 0xFF)));
    CHECK(EXPECT(&g, 0, P(1, 0x09, 1, 2, 3, 4), P(1, 0, 0x09)));
    CHECK(EXPECT(&g, 0, P(1, 0x10), P(1, 0, 0x10, 0x00)));
    CHECK(EXPECT(h, 0, P(1, 0x12, 0x01), P(1, 0, 0x12)));
    CHECK(h->ctl.mem[0xA2] & 0x20);
    CHECK(EXPECT(h, 0, P(1, 0x0E, 0x12, 0x34), P(1, 0, 0x0E)));
    CHECK(EXPECT(h, 0, P(1, 0x22, 0x50, 0x00), P(2, 5, 1, 0x22)));       /* no IIC device */
    CHECK(EXPECT(h, 0, P(1, 0x23, 0x01), P(1, 0, 0x23)));
    CHECK(EXPECT(h, 0, P(1, 0x24, 0x05), P(1, 0, 0x24)));
    CHECK_EQ(h->ctl.mem[0xA6], 5);
    CHECK(EXPECT(h, 0, P(1, 0x21, 0x05), P(2, 2, 1, 0x21)));
    /* The power key's packet once pseudo-command $21 1 armed it; once. */
    CHECK(EXPECT(h, 0, P(1, 0x21, 0x01), P(1, 0, 0x21)));
    mc_adbctl_power_key(&h->ctl, 1);
    const ac_packet *r = ac_next(h, AC_MS(20));
    CHECK(suite_is(r, P(4, 4, 4), 3));
    mc_adbctl_power_key(&h->ctl, 0);
    mc_adbctl_power_key(&h->ctl, 1);
    ac_run(h, AC_MS(20));
    CHECK_EQ(h->nlog, h->read);
    mc_adbctl_power_key(&h->ctl, 0);
    CHECK(EXPECT(h, 0, P(1, 0x21, 0x03), P(1, 0, 0x21)));
    CHECK(!(h->ctl.mem[0xA1] & 0x02));
    CHECK(EXPECT(h, 0, P(1, 0x21, 0x00), P(1, 0, 0x21)));
    /* TIP negated with BYTEACK low: Cuda resynchronises (TREQ, 8 clocks,
     * BYTEACK high, TREQ negated, 8 clocks). */
    mc_via_ops.write(&h->via, MC_VIA_ORB << 9, 1, 0x20);
    ac_run(h, AC_MS(1));
    CHECK(!(mc_via_ops.read(&h->via, MC_VIA_ORB << 9, 1) & 0x08));
    mc_via_ops.write(&h->via, MC_VIA_ORB << 9, 1, 0x30);
    ac_run(h, AC_MS(1));
    CHECK(mc_via_ops.read(&h->via, MC_VIA_ORB << 9, 1) & 0x08);
    CHECK(EXPECT(h, 0, P(1, 0x16), P(1, 0, 0x16, 0x0B)));
    /* Firmware identification by size and CRC32. */
    static uint8_t zero[MC_ADBCTL_FW_SIZE];
    mc_adbctl_kind kind;
    CHECK(mc_adbctl_firmware_name(zero, sizeof zero, &kind) == NULL);
    CHECK(!mc_adbctl_use_firmware(&g.ctl, zero, sizeof zero));
}

/* G3's IIC packets travel through the same VIA link as legacy commands. */
static void spd_packets(void){
    ac_host *h=adbctl_suite_host();mc_sdram_spd spd;uint32_t banks[8];
    CHECK(mc_sdram_spd_init(&spd,64u<<20,banks));h->ctl.iic_ctx=&spd;h->ctl.iic_transfer=mc_sdram_spd_transfer;
    CHECK(EXPECT(h,0,P(1,0x25,0xA0,2,0xA1),P(1,0,0x25,4,12,9,1)));
    CHECK(EXPECT(h,0,P(1,0x25,0xA2,2,0xA3),P(2,5,1,0x25)));
    CHECK(EXPECT(h,0,P(1,0x22,0xA0,2),P(1,0,0x22)));
    CHECK(EXPECT(h,0,P(1,0x22,0xA1),P(1,0,0x22,4)));
    h->ctl.iic_transfer=NULL;h->ctl.iic_ctx=NULL;
    CHECK(EXPECT(h,0,P(1,0x25,0xA0,2,0xA1),P(2,2,1,0x25)));
}

int main(void) {
    adbctl_suite(MC_ADBCTL_CUDA);
    cuda_commands(adbctl_suite_host());
    spd_packets();
    printf("test_cuda: %d failure(s)\n", mc_test_failures);
    return mc_test_failures ? 1 : 0;
}
