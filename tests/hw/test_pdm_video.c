/* test_pdm_video: the PDM boards' built-in video (pdm_video.h) and its CLUT
 * DAC Ariel II (ariel.h): the sense lines of each monitor as the ROM reads
 * them, the blanking line's timing, the view, and the DAC's address and
 * data registers as the POST writes them. */
#include "hw_test.h"
#include "ppc/chips/ariel.h"
#include "ppc/chips/pdm_video.h"

static hw_fake fake;

/* The ROM's extended sense ($FFCCDBBE-$FFCCDC34). */
static unsigned sense_code(mc_pdm_video *v) {
    mc_pdm_video_ops.write(v, 2, 1, 7);
    const unsigned std = (mc_pdm_video_ops.read(v, 2, 1) >> 4) & 7u;
    if (std != 7) return std;
    unsigned code = 0;
    mc_pdm_video_ops.write(v, 2, 1, 3);
    code |= ((mc_pdm_video_ops.read(v, 2, 1) >> 4) & 3u) << 4;
    mc_pdm_video_ops.write(v, 2, 1, 5);
    unsigned b = (mc_pdm_video_ops.read(v, 2, 1) >> 4) & 5u;
    code |= ((b & 1u) | (b >> 1 & 2u)) << 2;
    mc_pdm_video_ops.write(v, 2, 1, 6);
    code |= ((mc_pdm_video_ops.read(v, 2, 1) >> 4) & 6u) >> 1;
    return 0x40u | code;
}

static void test_sense(void) {
    static const struct { mc_pdm_monitor m; unsigned code; } cases[] = {
        {MC_PDM_MONITOR_RGB12, 2}, {MC_PDM_MONITOR_RGB13, 6}, {MC_PDM_MONITOR_PORTRAIT, 1},
        {MC_PDM_MONITOR_RGB16, 0x40 | 0x2D}, {MC_PDM_MONITOR_VGA, 0x40 | 0x17}, {MC_PDM_MONITOR_NONE, 0x40 | 0x3F}};
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        mc_pdm_video v;
        hw_fake_init(&fake);                                   /* each instance its own host */
        mc_pdm_video_init(&v, &fake.host, "video", cases[i].m);
        CHECK_EQ(sense_code(&v), cases[i].code);
    }
    CHECK_EQ(mc_pdm_video_monitor_for(832, 624), MC_PDM_MONITOR_RGB16);
    CHECK_EQ(mc_pdm_video_max_depth(MC_PDM_MONITOR_RGB13), 16);      /* Table 3-8 */
    CHECK_EQ(mc_pdm_video_max_depth(MC_PDM_MONITOR_PORTRAIT), 8);
}

static void test_timing(void) {
    mc_pdm_video v;
    hw_fake_init(&fake);
    mc_pdm_video_init(&v, &fake.host, "video", MC_PDM_MONITOR_RGB13);
    /* 13-inch: 896 x 525 dots of 31.3344 MHz, 66.6 Hz; 45 blanking lines. */
    const uint64_t frame = (uint64_t)MC_C15M_HZ * 896u * 525u / 31334400u;
    const unsigned before = hw_fake_changes(&fake, &v, MC_PDM_VIDEO_LINE_VBL);
    hw_fake_step(&fake, frame * 10u);
    const unsigned edges = hw_fake_changes(&fake, &v, MC_PDM_VIDEO_LINE_VBL) - before;
    CHECK(edges >= 19 && edges <= 21);
    mc_pdm_video_view view;
    CHECK(!mc_pdm_video_view_get(&v, &view));                 /* held off after reset */
    mc_pdm_video_set_base(&v, 0x00F41000u);
    mc_pdm_video_ops.write(&v, 0, 1, 0x1F);
    mc_pdm_video_ops.write(&v, 1, 1, 3);                       /* 8 bits */
    CHECK(mc_pdm_video_view_get(&v, &view));
    CHECK_EQ(view.width, 640);
    CHECK_EQ(view.depth, 8);
    CHECK_EQ(view.row_bytes, 640);
    CHECK_EQ(view.base, 0x00F41000u);
    mc_pdm_video_ops.write(&v, 1, 1, 5);                       /* not a depth */
    CHECK(!mc_pdm_video_view_get(&v, &view));
    CHECK_EQ(mc_pdm_video_ops.read(&v, 0, 1), 0x1F);
}

static void test_ariel(void) {
    mc_ariel a;
    mc_ariel_init(&a, &fake.host, "Ariel");
    /* The POST ($FFF03604-$FFF03664). */
    mc_ariel_ops.write(&a, 2, 1, 8);
    mc_ariel_ops.write(&a, 0, 1, 0x7F);
    for (unsigned i = 0; i < 3; i++) mc_ariel_ops.write(&a, 1, 1, 0xFF);
    mc_ariel_ops.write(&a, 0, 1, 0xFF);
    for (unsigned i = 0; i < 3; i++) mc_ariel_ops.write(&a, 1, 1, 0);
    CHECK_EQ(mc_ariel_palette(&a)[0x7F], 0xFFFFFFFFu);
    CHECK_EQ(mc_ariel_palette(&a)[0xFF], 0xFF000000u);
    CHECK_EQ(mc_ariel_ops.read(&a, 2, 1), 8);
    CHECK_EQ(mc_ariel_ops.read(&a, 0, 1), 0x00);              /* advanced past $FF */
    mc_ariel_ops.write(&a, 0, 1, 0x7F);
    CHECK_EQ(mc_ariel_ops.read(&a, 1, 1), 0xFF);
    /* One bit per pixel: values 0 and 1 reach entries $7F and $FF. */
    uint32_t pal[2];
    mc_ariel_view_palette(&a, 1, pal);
    CHECK_EQ(pal[0], 0xFFFFFFFFu);
    CHECK_EQ(pal[1], 0xFF000000u);
}

int main(void) {
    hw_fake_init(&fake);
    test_sense();
    test_timing();
    test_ariel();
    printf("test_pdm_video: %d failure(s)\n", mc_test_failures);
    return mc_test_failures ? 1 : 0;
}
