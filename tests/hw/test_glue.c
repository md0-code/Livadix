/* test_glue: the GLUE of the Macintosh II family and the SE/30
 * (asic/glue.c), the boards built on it (machines/macii.c), the SE/30's
 * video (video/se30video.c) and the Livadix Display Card in 24-bit mode.
 *
 * Livadix's own tests, from the Guide to the Macintosh Family Hardware, 2nd
 * ed. (asic/glue.h): the bank populations; the reset overlay; the RAM
 * space modulo twice bank A's window, which the VIA2 RAM size bits set
 * (Tables 4-9 and 4-10), checked by running the Macintosh II ROMs' own
 * sizing procedure (ROM offset $35A4, its two tables) over the decode for
 * every population; the ROM through $4000 0000-$4FFF FFFF; the undecoded
 * I/O space and $F0xx xxxx bus-erroring. Then the boards: the I/O block
 * repeated every $2 0000 up to $50FF FFFF, an unused offset of it
 * acknowledged, VIA1 PA4 releasing the overlay, VIA2 PA7/PA6 moving bank
 * B, VIA2 PB3 driving the AMU (low 24-bit, high or an input 32-bit), the
 * identification inputs the ROM 97221136 reads (VIA1 PA6 and VIA2 PB3),
 * the soft power line; the Livadix Display Card as a 24-bit card in slot $9
 * (its frame buffer and declaration ROM at $F990 0000 and $F99F FFFF as
 * well as $F900 0000 and $F9FF FFFF, through the AMU's 24-bit map at
 * $90 0000 and $9F FFFF); the SE/30's video: its RAM and ROM through slot
 * $E's space, the page select and the latched vertical blanking interrupt
 * (Guide, chapter 12, Tables 4-5 and 4-14). */
#include "asic/glue.h"
#include "bus.h"
#include "hw_machine.h"
#include "hw_test.h"
#include "livadix_card.h"
#include "via.h"
#include "video/se30video.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ROM_SIZE (256u << 10)

static uint32_t rd(mc_bus *b, uint32_t a, unsigned size, int *fault) {
    uint32_t v = 0;
    *fault = mc_bus_read(b, a, size, &v);
    return v;
}

static void test_populations(void) {
    uint32_t a = 0, b = 0;
    CHECK(mc_glue_banks(4, &a, &b) && a == 4u << 20 && b == 0);
    CHECK(mc_glue_banks(5, &a, &b) && a == 4u << 20 && b == 1u << 20);     /* the larger SIMMs in bank A */
    CHECK(mc_glue_banks(8, &a, &b) && a == 4u << 20 && b == 4u << 20);
    CHECK(mc_glue_banks(68, &a, &b) && a == 64u << 20 && b == 4u << 20);
    CHECK(mc_glue_banks(128, &a, &b) && a == 64u << 20 && b == 64u << 20);
    CHECK(!mc_glue_banks(6, &a, &b));
    CHECK(!mc_glue_banks(256, &a, &b));
    for (const uint16_t *r = mc_glue_ram_mb; *r; r++) {
        CHECK(mc_glue_banks(*r, &a, &b));
        CHECK_EQ((a + b) >> 20, *r);
        CHECK(a >= b);
    }
}

/* The Macintosh II ROMs' RAM sizing (ROM offset $35A4 in all three; the
 * tables at $3664 and $366E, the alias addresses at $3682): bank A's ICs
 * from the first window whose test address does not show at 0, then the
 * total from the first address of the list that shows at its alias, or
 * does not keep the eight walking patterns in any byte lane. */
static int ram_test(mc_bus *bus, uint32_t mb, uint32_t alias) {
    const uint32_t a = mb << 20;
    uint32_t d2 = (mb & 0xFFu) * 0x01010101u;
    mc_bus_write(bus, alias, 4, 0);
    for (int i = 0; i < 8; i++) {
        mc_bus_write(bus, a, 4, d2);
        uint32_t v = 0;
        mc_bus_read(bus, alias, 4, &v);
        if (v) return 0;
        mc_bus_write(bus, 4, 4, 0xFFFFFFFFu);     /* the bus driven high between */
        mc_bus_read(bus, a, 4, &v);
        int lane = 0;
        for (int k = 0; k < 4; k++) lane |= ((v >> (8 * k)) & 0xFFu) == ((d2 >> (8 * k)) & 0xFFu);
        if (!lane) return 0;
        d2 = d2 << 1 | d2 >> 31;
    }
    return 1;
}

static uint32_t rom_sizing(mc_glue *g, mc_bus *bus, uint8_t *ram, const uint8_t *rom) {
    static const uint8_t first[][2] = {{0x20, 0xC0}, {0x08, 0x80}, {0x02, 0x40}, {0x01, 0x00}};
    static const uint8_t list[] = {0x01, 0x02, 0x04, 0x05, 0x08, 0x10, 0x11, 0x14, 0x20, 0x40, 0x41, 0x44, 0x50, 0x80};
    static const uint32_t alias[] = {0, 0, 0, 0x00400000u, 0, 0, 0x01000000u, 0x01000000u, 0, 0, 0x04000000u, 0x04000000u,
                                     0x04000000u, 0};
    for (size_t i = 0; i < 4; i++) {
        if (mc_glue_set_size_bits(g, first[i][1])) mc_glue_map_ram(g, bus, ram, rom, ROM_SIZE);
        if (i == 3 || ram_test(bus, first[i][0], 0)) break;
    }
    for (size_t i = 0; i < sizeof list; i++)
        if (!ram_test(bus, list[i], alias[i])) return (uint32_t)list[i] << 20;
    return 128u << 20;
}

static void test_decode(void) {
    mc_bus bus;
    CHECK(mc_bus_init(&bus));
    uint8_t *rom = calloc(1, ROM_SIZE), *ram = calloc(1, 128u << 20);
    if (!rom || !ram) return;
    for (uint32_t i = 0; i < ROM_SIZE; i++) rom[i] = (uint8_t)(i * 7u + 3u);
    mc_glue g;
    mc_glue_init(&g, 4u << 20, 4u << 20);
    int fault = 0;
    /* From reset the ROM answers through the whole RAM space. */
    CHECK(g.overlay);
    mc_glue_map_fixed(&g, &bus, rom, ROM_SIZE);
    mc_glue_map_ram(&g, &bus, ram, rom, ROM_SIZE);
    CHECK_EQ(rd(&bus, 0x10, 1, &fault), rom[0x10]);
    CHECK_EQ(rd(&bus, 0x3FFC0010u, 1, &fault), rom[0x10]);
    CHECK(!fault);
    /* The ROM through its range, writes ignored. */
    CHECK_EQ(rd(&bus, 0x40800004u, 1, &fault), rom[4]);
    CHECK_EQ(rd(&bus, 0x4FFC0004u, 1, &fault), rom[4]);
    CHECK(mc_bus_write(&bus, 0x40800004u, 1, 0x55) == 0);
    CHECK_EQ(rd(&bus, 0x40800004u, 1, &fault), rom[4]);
    CHECK(!fault);
    /* Undecoded I/O space and the NuBus addresses of the board itself. */
    rd(&bus, 0x51000000u, 4, &fault);
    CHECK(fault);
    rd(&bus, 0x58000000u, 4, &fault);
    CHECK(fault);
    rd(&bus, 0xF0800000u, 4, &fault);
    CHECK(fault);
    /* The overlay off, the size bits for 1 Mbit ICs: a 4 MB window. */
    g.overlay = 0;
    CHECK(mc_glue_set_size_bits(&g, 0x40));
    CHECK_EQ(mc_glue_window(&g), 4u << 20);
    mc_glue_map_ram(&g, &bus, ram, rom, ROM_SIZE);
    CHECK(mc_bus_write(&bus, 0x100, 4, 0x11223344u) == 0);
    CHECK_EQ(ram[0x100], 0x11);
    CHECK(mc_bus_write(&bus, 0x00400200u, 4, 0xCAFEF00Du) == 0);
    CHECK_EQ(ram[(4u << 20) + 0x200], 0xCA);
    /* The space repeats every 8 MB: bank A, then bank B. */
    CHECK_EQ(rd(&bus, 0x00800100u, 4, &fault), 0x11223344u);
    CHECK_EQ(rd(&bus, 0x00C00200u, 4, &fault), 0xCAFEF00Du);
    CHECK_EQ(rd(&bus, 0x3FC00200u, 4, &fault), 0xCAFEF00Du);
    CHECK(!fault);
    /* A 64 MB window over 4 MB of ICs: bank A repeats inside it. */
    CHECK(mc_glue_set_size_bits(&g, 0xC0));
    mc_glue_map_ram(&g, &bus, ram, rom, ROM_SIZE);
    CHECK_EQ(rd(&bus, 0x02000100u, 4, &fault), 0x11223344u);
    CHECK_EQ(rd(&bus, 0x04000200u, 4, &fault), 0xCAFEF00Du);
    /* An empty bank B floats: the cycle completes, nothing is kept. */
    mc_glue_init(&g, 4u << 20, 0);
    g.overlay = 0;
    mc_glue_set_size_bits(&g, 0x40);
    mc_glue_map_ram(&g, &bus, ram, rom, ROM_SIZE);
    CHECK(mc_bus_write(&bus, 0x00400200u, 4, 0x12345678u) == 0);
    CHECK_EQ(rd(&bus, 0x00400200u, 4, &fault), 0);
    CHECK(!fault);
    /* The ROMs' sizing finds every population. */
    for (const uint16_t *r = mc_glue_ram_mb; *r; r++) {
        uint32_t a = 0, b = 0;
        mc_glue_banks(*r, &a, &b);
        mc_glue_init(&g, a, b);
        g.overlay = 0;
        g.size_bits = 3;
        memset(ram, 0, 128u << 20);
        mc_glue_map_ram(&g, &bus, ram, rom, ROM_SIZE);
        const uint32_t total = rom_sizing(&g, &bus, ram, rom);
        if (total != (uint32_t)*r << 20) printf("  %u MB sized as %u MB\n", (unsigned)*r, (unsigned)(total >> 20));
        CHECK_EQ(total, (uint32_t)*r << 20);
        CHECK_EQ(mc_glue_window(&g), a);
    }
    mc_bus_free(&bus);
    free(rom);
    free(ram);
}

/* A machine built around a ROM image that only carries its checksum (the
 * CPU does not run). */
static mc_machine *machine(const char *model, uint32_t checksum, uint32_t ram_mb, uint8_t **rom_out) {
    uint8_t *rom = calloc(1, ROM_SIZE);
    if (!rom) return NULL;
    for (int i = 0; i < 4; i++) rom[i] = (uint8_t)(checksum >> (24 - 8 * i));
    mc_config c;
    mc_config_defaults(&c);
    c.model = (uint32_t)mc_model_index(mc_model_find(model));
    c.ram_mb = ram_mb;
    c.width = 640; c.height = 480; c.depth = 8;
    c.disk_count = 0;
    c.nvram[0] = 0;
    char error[256] = "";
    mc_machine *m = mc_machine_create(&c, rom, ROM_SIZE, error, sizeof error);
    if (!m) printf("%s: %s\n", model, error);
    *rom_out = rom;
    return m;
}

static void wr(mc_bus *b, uint32_t a, uint8_t v) { CHECK(mc_bus_write(b, a, 1, v) == 0); }

static void test_board_io(void) {
    uint8_t *rom = NULL;
    mc_machine *m = machine("mac-iix", 0x97221136u, 8, &rom);
    CHECK(m != NULL);
    if (!m) { free(rom); return; }
    mc_hw *hw = mc_machine_hw(m);
    CHECK(hw && hw->cpu->model == M68K_68030);
    mc_bus *bus = &hw->bus;
    int fault = 0;
    /* VIA1's IER in every image of the block ($2 0000 apart), VIA2's. */
    wr(bus, 0x50001C00u, 0x82);
    CHECK_EQ(rd(bus, 0x50F01C00u, 1, &fault), 0x82);
    CHECK_EQ(rd(bus, 0x50FE1C00u, 1, &fault), 0x82);
    CHECK_EQ(rd(bus, 0x50021C00u, 1, &fault), 0x82);
    wr(bus, 0x50F03C00u, 0x90);
    CHECK_EQ(rd(bus, 0x50003C00u, 1, &fault), 0x90);
    CHECK_EQ(rd(bus, 0x50F01C00u, 1, &fault), 0x82);
    CHECK(!fault);
    /* An offset of the block no device decodes: acknowledged, reads 0. */
    CHECK_EQ(rd(bus, 0x50F08000u, 1, &fault), 0);
    CHECK_EQ(rd(bus, 0x50F1A000u, 1, &fault), 0);
    CHECK(!fault);
    rd(bus, 0x51000000u, 1, &fault);
    CHECK(fault);
    /* The ASC's version register (the original ASC). */
    CHECK_EQ(rd(bus, 0x50F14800u, 1, &fault), 0x00);
    CHECK(!fault);
    /* The overlay from reset: the ROM's checksum at 0. */
    CHECK_EQ(rd(bus, 0, 4, &fault), 0x97221136u);
    /* VIA1 PA4 driven low releases it. */
    wr(bus, 0x50F00600u, 0x10);        /* DDRA: PA4 an output */
    wr(bus, 0x50F01E00u, 0x00);
    CHECK(mc_bus_write(bus, 0x100, 4, 0x5A5A1234u) == 0);
    CHECK_EQ(rd(bus, 0x100, 4, &fault), 0x5A5A1234u);
    CHECK_EQ(hw->ram[0x100], 0x5A);
    /* PA4 high again: the overlay. */
    wr(bus, 0x50F01E00u, 0x10);
    CHECK_EQ(rd(bus, 0x100, 1, &fault), 0x00);
    wr(bus, 0x50F01E00u, 0x00);
    /* VIA2 PA7/PA6 set bank A's window: bank B (host RAM from 4 MB) at
     * 4 MB with 1 Mbit ICs, at 16 MB with 4 Mbit ones. */
    wr(bus, 0x50F02600u, 0xC0);        /* DDRA: PA7/PA6 outputs */
    wr(bus, 0x50F03E00u, 0x40);
    CHECK(mc_bus_write(bus, 0x00400010u, 4, 0x0B0B0B0Bu) == 0);
    CHECK_EQ(hw->ram[(4u << 20) + 0x10], 0x0B);
    wr(bus, 0x50F03E00u, 0x80);
    CHECK_EQ(rd(bus, 0x01000010u, 4, &fault), 0x0B0B0B0Bu);
    CHECK_EQ(rd(bus, 0x00400010u, 4, &fault), 0);   /* bank A's 4 MB repeat: the same as 0x10 */
    /* Identification: CPU.ID0 (VIA1 PA6) low and PB3 low, the IIx. */
    CHECK_EQ(rd(bus, 0x50F01E00u, 1, &fault) & 0x40u, 0);
    CHECK_EQ(rd(bus, 0x50F02000u, 1, &fault) & 0x08u, 0);
    /* Empty NuBus slots bus-error; the card is in slot $9. */
    rd(bus, 0xFAFFFFFCu, 4, &fault);
    CHECK(fault);
    rd(bus, 0xEFFFFFFCu, 4, &fault);
    CHECK(fault);
    mc_machine_destroy(m);
    free(rom);
    /* The other boards' identification and the IIcx's three slots. */
    static const struct { const char *model; uint32_t rom; int pa6, pb3; } ids[] = {
        {"mac-ii-v2", 0x9779D2C4u, 0, 1}, {"mac-ii-fdhd", 0x97221136u, 0, 1}, {"mac-iicx", 0x97221136u, 1, 1},
    };
    for (size_t i = 0; i < sizeof ids / sizeof *ids; i++) {
        m = machine(ids[i].model, ids[i].rom, 8, &rom);
        CHECK(m != NULL);
        if (!m) { free(rom); continue; }
        hw = mc_machine_hw(m);
        bus = &hw->bus;
        CHECK_EQ((rd(bus, 0x50F01E00u, 1, &fault) >> 6) & 1u, (uint32_t)ids[i].pa6);
        CHECK_EQ((rd(bus, 0x50F02000u, 1, &fault) >> 3) & 1u, (uint32_t)ids[i].pb3);
        CHECK_EQ(hw->cpu->model, !strcmp(ids[i].model, "mac-iicx") ? M68K_68030 : M68K_68020);
        if (!strcmp(ids[i].model, "mac-iicx")) {
            rd(bus, 0xFCFFFFFCu, 4, &fault);   /* no slot $C on the IIcx */
            CHECK(fault);
        }
        mc_machine_destroy(m);
        free(rom);
    }
}

/* The Macintosh II's AMU on VIA2 PB3 and the card in its 24-bit window. */
static void test_amu_and_card(void) {
    uint8_t *rom = NULL;
    mc_machine *m = machine("mac-ii-v2", 0x9779D2C4u, 8, &rom);
    CHECK(m != NULL);
    if (!m) { free(rom); return; }
    mc_hw *hw = mc_machine_hw(m);
    mc_bus *bus = &hw->bus;
    int fault = 0;
    CHECK(hw->cpu->model == M68K_68020);
    /* PB3 an input after reset floats high: 32 bits. */
    CHECK(!hw->amu.on);
    wr(bus, 0x50F02400u, 0x08);        /* DDRB: PB3 an output, low */
    wr(bus, 0x50F02000u, 0x00);
    CHECK(hw->amu.on);
    wr(bus, 0x50F02000u, 0x08);
    CHECK(!hw->amu.on);
    wr(bus, 0x50F02000u, 0x00);
    CHECK(hw->amu.on);
    wr(bus, 0x50F02400u, 0x00);        /* back to an input */
    CHECK(!hw->amu.on);
    /* The card's frame buffer from $F900 0000 and again at $F990 0000. */
    CHECK(mc_bus_write(bus, 0xF9000040u, 4, 0x13579BDFu) == 0);
    CHECK_EQ(rd(bus, 0xF9900040u, 4, &fault), 0x13579BDFu);
    CHECK(mc_bus_write(bus, 0xF99EFFFCu, 4, 0x2468ACE0u) == 0);
    CHECK_EQ(rd(bus, 0xF90EFFFCu, 4, &fault), 0x2468ACE0u);
    CHECK(!fault);
    /* Its declaration ROM's last byte (ByteLanes $0F) at the top of the
     * 16 MB slot, of the $Fss0 0000 MB and of the 24-bit window. */
    CHECK_EQ(rd(bus, 0xF9FFFFFFu, 1, &fault), 0x0F);
    CHECK_EQ(rd(bus, 0xF99FFFFFu, 1, &fault), 0x0F);
    CHECK_EQ(rd(bus, 0xF90FFFFFu, 1, &fault), 0x0F);
    CHECK(!fault);
    /* Through the AMU's 24-bit map: $90 0000 is the frame buffer, $9F FFFF
     * the ROM's top, $F0 0000 the I/O block. */
    CHECK_EQ(mc_amu_map(&hw->amu, 0x00900040u), 0xF9000040u);
    CHECK_EQ(rd(bus, mc_amu_map(&hw->amu, 0x00900040u), 4, &fault), 0x13579BDFu);
    CHECK_EQ(rd(bus, mc_amu_map(&hw->amu, 0x009FFFFFu), 1, &fault), 0x0F);
    CHECK_EQ(mc_amu_map(&hw->amu, 0x00F01C00u), 0x50001C00u);
    /* The card's registers at its 32-bit address: its ID. */
    CHECK_EQ(rd(bus, 0xF9C00000u, 4, &fault), 0x4C564458u);   /* 'LVDX' */
    CHECK(!fault);
    /* v2PowerOff low switches the Macintosh II off. */
    CHECK(!mc_machine_powered_off(m));
    wr(bus, 0x50F02400u, 0x04);
    wr(bus, 0x50F02000u, 0x00);
    CHECK(mc_machine_powered_off(m));
    mc_machine_destroy(m);
    free(rom);
}

static void test_se30_video(void) {
    hw_fake f;
    hw_fake_init(&f);
    uint8_t rom[MC_SE30VIDEO_ROM];
    for (unsigned i = 0; i < sizeof rom; i++) rom[i] = (uint8_t)(i ^ 0xA5u);
    rom[sizeof rom - 1] = 0x0F;
    static mc_se30video v;
    CHECK(!mc_se30video_init(&v, &f.host, "Video", rom, 4096));
    CHECK(mc_se30video_init(&v, &f.host, "Video", rom, sizeof rom));
    f.dev[0] = &v;
    mc_bus bus;
    CHECK(mc_bus_init(&bus));
    mc_se30video_map(&v, &bus);
    int fault = 0;
    /* The video RAM through slot $E's space, every 64 KB. */
    CHECK(mc_bus_write(&bus, 0xFEE08040u, 4, 0xF0F0F0F0u) == 0);
    CHECK_EQ(v.vram[0x8040], 0xF0);
    CHECK_EQ(rd(&bus, 0xFE008040u, 4, &fault), 0xF0F0F0F0u);
    CHECK_EQ(rd(&bus, 0xFE018040u, 4, &fault), 0xF0F0F0F0u);
    /* The ROM in the last 64 KB of each MB, the 24-bit window's and the
     * slot's top. */
    CHECK_EQ(rd(&bus, 0xFEFFFFFFu, 1, &fault), 0x0F);
    CHECK_EQ(rd(&bus, 0xFE0FFFFFu, 1, &fault), 0x0F);
    CHECK_EQ(rd(&bus, 0xFEFFE000u, 1, &fault), rom[0]);
    CHECK(mc_bus_write(&bus, 0xFEFFE000u, 1, 0x00) == 0);
    CHECK_EQ(rd(&bus, 0xFEFFE000u, 1, &fault), rom[0]);
    CHECK(!fault);
    /* PA6 selects the main ($8040) or alternate ($0040) buffer. */
    mc_se30video_ops.reset(&v);
    mc_se30video_view view;
    mc_se30video_view_get(&v, &view);
    CHECK(view.pixels == v.vram + MC_SE30VIDEO_MAIN);
    CHECK_EQ(view.palette[0], 0xFFFFFFFFu);
    CHECK_EQ(view.palette[1], 0xFF000000u);
    mc_se30video_set_page(&v, 0);
    mc_se30video_view_get(&v, &view);
    CHECK(view.pixels == v.vram + MC_SE30VIDEO_ALT);
    /* The vertical blanking every 704 x 370 C15M periods: no interrupt
     * while PB6 is high, latched while it is low, cleared by PB6 high. */
    hw_fake_advance(&f, 704u * 370u + 1u);
    CHECK_EQ(hw_fake_line(&f, &v, MC_SE30VIDEO_LINE_IRQ), 0);
    mc_se30video_set_sync_enable(&v, 0);
    hw_fake_advance(&f, 2u * 704u * 370u - 1u);
    CHECK_EQ(hw_fake_line(&f, &v, MC_SE30VIDEO_LINE_IRQ), 0);
    hw_fake_advance(&f, 2u * 704u * 370u);
    CHECK_EQ(hw_fake_line(&f, &v, MC_SE30VIDEO_LINE_IRQ), 1);
    hw_fake_advance(&f, 3u * 704u * 370u);
    CHECK_EQ(hw_fake_changes(&f, &v, MC_SE30VIDEO_LINE_IRQ), 1u);
    mc_se30video_set_sync_enable(&v, 1);
    CHECK_EQ(hw_fake_line(&f, &v, MC_SE30VIDEO_LINE_IRQ), 0);
    mc_se30video_set_sync_enable(&v, 0);
    hw_fake_advance(&f, 4u * 704u * 370u);
    CHECK_EQ(hw_fake_line(&f, &v, MC_SE30VIDEO_LINE_IRQ), 1);
    mc_bus_free(&bus);
}

int main(void) {
    test_populations();
    test_decode();
    test_board_io();
    test_amu_and_card();
    test_se30_video();
    printf("test_glue: %d failure(s)\n", mc_test_failures);
    return mc_test_failures != 0;
}
