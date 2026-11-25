/* test_vasp: VASP (asic/vasp.c), the IIvx, IIvi and Performa 600's
 * integrated controller, and the board's empty FPU socket.
 *
 * Livadix's own tests, from the Macintosh IIvx Developer Note (Table 1-3, the
 * video modes and VRAM; Figures 1-2 and 1-3, the frame timing; "Features",
 * the FPU optional on the IIvi and Performa 600) and the IIvx ROM's use of
 * the registers (vasp.h): the pseudo-VIA2's enables and flags, the slot
 * lines of NuBus slots $C-$E active low, the video's line through each
 * vertical blanking interval, the 60.15 Hz tick, the monitor parameters
 * (depth, sense code, video off), rows of 1024 or 2048 bytes by the VRAM
 * fitted, the CLUT DAC; the boards' FPU sockets. */
#include "asic/vasp.h"
#include "hw_machine.h"
#include "hw_test.h"
#include "models.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static hw_fake f;
static mc_vasp v;
static uint8_t vram[MC_VASP_VRAM_MAX];

static uint8_t rd(uint32_t reg) { return (uint8_t)mc_vasp_ops.read(&v, reg, 1); }
static void wr(uint32_t reg, uint8_t val) { mc_vasp_ops.write(&v, reg, 1, val); }
static int irq(void) { return hw_fake_line(&f, &v, MC_VASP_LINE_IRQ); }

static void setup(mc_vasp_monitor monitor, uint32_t vram_size) {
    hw_fake_init(&f);
    memset(vram, 0, sizeof vram);
    mc_vasp_init(&v, &f.host, "VASP", monitor, vram, vram_size);
}

static void test_interrupts(void) {
    setup(MC_VASP_NONE, MC_VASP_VRAM_STD);
    CHECK_EQ(rd(MC_VASP_IER), 0x80);
    mc_vasp_set_irq(&v, MC_VASP_IRQ_SCSI, 1);
    CHECK_EQ(rd(MC_VASP_IFR), MC_VASP_IRQ_SCSI);
    CHECK(!irq());
    wr(MC_VASP_IER, 0x88);
    CHECK(irq());
    CHECK_EQ(rd(MC_VASP_IFR), 0x80 | MC_VASP_IRQ_SCSI);
    mc_vasp_set_irq(&v, MC_VASP_IRQ_SCSI, 0);
    wr(MC_VASP_IFR, MC_VASP_IRQ_SCSI);
    CHECK(!irq());
    /* The POST's register test: seven enables, four slot enables (bits
     * 6-3). */
    wr(MC_VASP_IER, 0xFF);
    CHECK_EQ(rd(MC_VASP_IER), 0xFF);
    wr(MC_VASP_SIER, 0xFF);
    CHECK_EQ(rd(MC_VASP_SIER), 0xF8);
    /* The register block repeats: IFR at +$1A03 (the shared VIA2 code). */
    mc_vasp_set_irq(&v, MC_VASP_IRQ_SOUND, 1);
    CHECK_EQ((uint8_t)mc_vasp_ops.read(&v, 0x1A03, 1), 0x80 | MC_VASP_IRQ_SOUND);
}

static void test_slots(void) {
    setup(MC_VASP_NONE, MC_VASP_VRAM_STD);
    CHECK_EQ(rd(MC_VASP_SIFR), 0xFF);
    wr(MC_VASP_SIER, 0xFF);
    wr(MC_VASP_IER, 0x82);
    mc_vasp_set_slots(&v, 0x10, 0x38);      /* slot $D */
    CHECK_EQ(rd(MC_VASP_SIFR), 0xEF);
    CHECK(irq());
    mc_vasp_set_slots(&v, 0, 0x38);
    CHECK(!irq());
}

static void test_vbl_and_tick(void) {
    setup(MC_VASP_HIRES13, MC_VASP_VRAM_STD);
    /* The 13-inch: 896 x 525 dots of 31.3344 MHz, 45 lines of blanking
     * (Figure 1-3). */
    const uint64_t frame = (uint64_t)MC_C15M_HZ * 896u * 525u / 31334400u;
    const uint64_t blank = (uint64_t)MC_C15M_HZ * 896u * 45u / 31334400u;
    hw_fake_step(&f, 10);
    CHECK_EQ(rd(MC_VASP_SIFR) & 0x40, 0);
    hw_fake_step(&f, blank);
    CHECK_EQ(rd(MC_VASP_SIFR) & 0x40, 0x40);
    hw_fake_step(&f, frame - blank);
    CHECK_EQ(rd(MC_VASP_SIFR) & 0x40, 0);
    const unsigned before = hw_fake_changes(&f, &v, MC_VASP_LINE_TICK);
    hw_fake_step(&f, 704u * 370u * 4u);
    CHECK_EQ(hw_fake_changes(&f, &v, MC_VASP_LINE_TICK) - before, 8u);
}

static void test_video(void) {
    /* The sense code in bits 5-3 of $10, the video off in bit 6 at reset. */
    setup(MC_VASP_RGB12, MC_VASP_VRAM_STD);
    CHECK_EQ(rd(MC_VASP_MONP), 0x40 | (2 << 3));
    setup(MC_VASP_NONE, MC_VASP_VRAM_STD);
    CHECK_EQ(rd(MC_VASP_MONP) >> 3 & 7, 7);
    setup(MC_VASP_HIRES13, MC_VASP_VRAM_STD);
    CHECK_EQ(rd(MC_VASP_MONP) >> 3 & 7, 6);
    mc_vasp_view view;
    CHECK(!mc_vasp_view_get(&v, &view));
    /* 8 bits ($03, the PrimaryInit's start): rows of 1024 bytes with
     * 512 KB. */
    wr(MC_VASP_MONP, 0x03);
    CHECK(mc_vasp_view_get(&v, &view));
    CHECK(view.width == 640 && view.height == 480 && view.depth == 8 && view.row_bytes == 1024);
    wr(MC_VASP_MONP, 0x04);                 /* 16 bits need 1 MB */
    CHECK(!mc_vasp_view_get(&v, &view));
    setup(MC_VASP_HIRES13, MC_VASP_VRAM_MAX);
    wr(MC_VASP_MONP, 0x04);
    CHECK(mc_vasp_view_get(&v, &view));
    CHECK(view.depth == 16 && view.row_bytes == 2048);
    CHECK_EQ(mc_vasp_max_depth(MC_VASP_RGB12, MC_VASP_VRAM_STD), 16u);
    CHECK_EQ(mc_vasp_max_depth(MC_VASP_HIRES13, MC_VASP_VRAM_STD), 8u);
    CHECK_EQ(mc_vasp_max_depth(MC_VASP_HIRES13, MC_VASP_VRAM_MAX), 16u);
    /* The DAC, as Sonora's: address, red, green, blue, next address; a
     * 1-bit pixel reaches it in its top bit, the rest set. */
    mc_vasp_dac_ops.write(&v, 0, 1, 0x7F);
    for (int i = 0; i < 3; i++) mc_vasp_dac_ops.write(&v, 1, 1, 0xFF);
    for (int i = 0; i < 3; i++) mc_vasp_dac_ops.write(&v, 1, 1, 0x00);
    CHECK_EQ((uint8_t)mc_vasp_dac_ops.read(&v, 0, 1), 0x81);
    wr(MC_VASP_MONP, 0x00);
    CHECK(mc_vasp_view_get(&v, &view));
    CHECK(view.depth == 1 && view.palette[0] == 0xFFFFFFFFu && view.palette[1] == 0xFF000000u);
}

/* The FPU socket: the IIvx has its 68882, the IIvi and Performa 600 leave
 * it empty ("Features"); the ROM's coprocessor probe in CPU space then
 * bus-errors (m68k_run.c, the 68030's CPU space). */
static void test_fpu_socket(void) {
    CHECK_EQ(mc_hw_find("iivx")->fpu, (uint8_t)M68K_FPU_68882);
    CHECK_EQ(mc_hw_find("iivi")->fpu, (uint8_t)M68K_FPU_NONE);
    CHECK_EQ(mc_hw_find("performa-600")->fpu, (uint8_t)M68K_FPU_NONE);
}

int main(void) {
    test_interrupts();
    test_slots();
    test_vbl_and_tick();
    test_video();
    test_fpu_socket();
    printf("test_vasp: %d failure(s)\n", mc_test_failures);
    return mc_test_failures != 0;
}
