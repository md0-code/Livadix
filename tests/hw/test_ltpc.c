/* CHRP I/O 9.8.2-9.8.4 and 9.10.3: sixteen mark bits, separate
 * channels, latched detection, transmitter activity and RTS gating. */
#include "hw_test.h"
#include "ppc/chips/ltpc.h"
#include <stdio.h>

static hw_fake fake;
static mc_scc scc;
static mc_ltpc ltpc;
static unsigned checks;
#define T(x) do { CHECK(x); checks++; } while (0)

static void reg(unsigned channel, uint8_t number, uint8_t value) {
    const unsigned control = channel == MC_SCC_A ? 2u : 0u;
    mc_scc_ops.write(&scc, control, 1, number);
    mc_scc_ops.write(&scc, control, 1, value);
    mc_ltpc_changed(&ltpc, channel);
}

static void setup(void) {
    hw_fake_init(&fake);
    mc_scc_init(&scc, &fake.host);
    mc_scc_set_clocks(&scc, 3686400, 3686400, 3686400);
    mc_ltpc_init(&ltpc, &fake.host, &scc);
    for (unsigned channel = 0; channel < 2; channel++) {
        reg(channel, 4, 0x20); /* SDLC x1 */
        reg(channel, 5, 0x6A); /* eight data bits, transmitter and RTS */
        reg(channel, 10, 8);   /* mark idle */
        reg(channel, 11, 0x10); /* transmitter uses BRG */
        reg(channel, 12, 6);  /* 3.6864 MHz / 16 = 230.4 kbit/s */
        reg(channel, 13, 0);
        reg(channel, 14, 1);
    }
}

static void detection(void) {
    setup();
    const uint64_t marks = mc_scc_tx_time(&scc, MC_SCC_A, 16);
    T(marks == 1088);
    T(mc_ltpc_ops.read(&ltpc, MC_LTPC_DETECT, 1) == 0);
    mc_ltpc_ops.write(&ltpc, MC_LTPC_START_A, 1, 1);
    hw_fake_step(&fake, marks / 2);
    mc_ltpc_ops.write(&ltpc, MC_LTPC_START_B, 1, 1);
    T(mc_ltpc_ops.read(&ltpc, MC_LTPC_START_A, 1) == 1);
    T(mc_ltpc_ops.read(&ltpc, MC_LTPC_START_B, 1) == 1);
    reg(MC_SCC_A, 3, 0xC1); /* receiver setup preserves the A countdown */
    hw_fake_advance(&fake, marks - 1);
    T(ltpc.detect == 0);
    hw_fake_step(&fake, 1);
    T(ltpc.detect == 2 && mc_ltpc_tristated(&ltpc, MC_SCC_A));
    T(hw_fake_line(&fake, &ltpc, MC_LTPC_LINE_DETECT_A) == 1);
    hw_fake_step(&fake, marks / 2);
    T(ltpc.detect == 3);
    mc_ltpc_ops.write(&ltpc, MC_LTPC_START_A, 1, 1);
    T(ltpc.detect == 3); /* only clearing Start clears Detect */
    mc_ltpc_ops.write(&ltpc, MC_LTPC_START_A, 1, 0);
    T(ltpc.detect == 1 && !mc_ltpc_tristated(&ltpc, MC_SCC_A));
    T(hw_fake_line(&fake, &ltpc, MC_LTPC_LINE_DETECT_A) == 0);
    mc_ltpc_ops.reset(&ltpc);
    T(ltpc.start == 0 && ltpc.detect == 0);
    T(hw_fake_deadline(&fake, &ltpc) == MC_NEVER);
}

static void active_transmitter(void) {
    setup();
    mc_ltpc_ops.write(&ltpc, MC_LTPC_START_A, 1, 1);
    hw_fake_step(&fake, 500);
    T(mc_scc_dma_write(&scc, MC_SCC_A, 0x35));
    mc_ltpc_changed(&ltpc, MC_SCC_A);
    const uint64_t last_bit = scc.ch[MC_SCC_A].tx_done;
    const uint64_t marks = mc_scc_tx_time(&scc, MC_SCC_A, 16);
    hw_fake_advance(&fake, last_bit + marks - 1);
    T(ltpc.detect == 0);
    hw_fake_step(&fake, 1);
    T(ltpc.detect == 2);
    setup();
    reg(MC_SCC_B, 10, 0); /* flag idle has no abort sequence */
    mc_ltpc_ops.write(&ltpc, MC_LTPC_START_B, 1, 1);
    hw_fake_step(&fake, 5000);
    T(ltpc.detect == 0 && hw_fake_deadline(&fake, &ltpc) == MC_NEVER);
    reg(MC_SCC_B, 10, 8);
    reg(MC_SCC_B, 5, 0x68); /* RTS deasserted */
    hw_fake_step(&fake, 5000);
    T(ltpc.detect == 0);
    reg(MC_SCC_B, 5, 0x6A);
    hw_fake_step(&fake, 1088);
    T(ltpc.detect == 1);
    setup();
    reg(MC_SCC_A, 11, 8); /* no external TRxC clock */
    mc_ltpc_ops.write(&ltpc, MC_LTPC_START_A, 1, 1);
    hw_fake_step(&fake, 5000);
    T(ltpc.detect == 0 && hw_fake_deadline(&fake, &ltpc) == MC_NEVER);
}

int main(void) {
    detection();
    active_transmitter();
    printf("LocalTalk assist: %u checks, %d failures\n", checks, mc_test_failures);
    return mc_test_failures ? 1 : 0;
}
