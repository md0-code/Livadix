/* test_bart: the PDM boards' NuBus controller (bart.h): its registers as
 * the ROM writes them, slot decode and timeouts as the Slot Manager probes
 * them, the Livadix Display Card's declaration ROM and VRAM through a slot,
 * and the slots' interrupts. */
#include "hw_test.h"
#include "livadix_card.h"
#include "ppc/chips/bart.h"

static hw_fake fake;

static void test_registers(void) {
    mc_bart b;
    mc_bart_init(&b, &fake.host, "BART", 0x7000);
    mc_bart_ops.reset(&b);
    mc_bart_ops.write(&b, 0x00, 1, 0x80);                      /* $FFCC527C */
    mc_bart_ops.write(&b, 0x11, 1, 0x80);                      /* $FFCC5290 */
    CHECK_EQ(mc_bart_ops.read(&b, 0x00, 1), 0x80);
    CHECK_EQ(mc_bart_ops.read(&b, 0x10, 2), 0x0080);
    CHECK_EQ(mc_bart_ops.read(&b, 8, 4), 0x43184001u);
    mc_bart_ops.write(&b, 8, 4, 0);
    CHECK_EQ(mc_bart_ops.read(&b, 8, 4), 0x43184001u);
    b.id = MC_BART_ID_21;
    mc_bart_ops.reset(&b);
    CHECK_EQ(mc_bart_ops.read(&b, 8, 2), 0x4318);
    CHECK_EQ(mc_bart_ops.read(&b, 11, 1), 3);
}

static void test_slots(void) {
    mc_bart b;
    mc_bart_init(&b, &fake.host, "BART", 0x7000);              /* slots $C-$E */
    uint32_t v;
    CHECK(!mc_bart_read(&b, 0xFEFFFF00u, 1, &v));              /* empty: a timeout ($FFCC52A4) */
    CHECK(!mc_bart_fit(&b, 0x9));                              /* not a slot of this board */
    CHECK_EQ(mc_bart_free_slot(&b), 0xC);
    mc_livadix_card card;
    uint8_t rom[64] = {0};
    rom[63] = 0x0F;                                            /* all four byte lanes */
    CHECK(mc_livadix_card_init(&card, &fake.host, "card", 2u * 1024u * 1024u, 0, rom, sizeof rom, 0));
    CHECK(mc_bart_fit(&b, 0xC));
    CHECK(mc_bart_map_memory(&b, 0xC, MC_LIVADIX_CARD_VRAM_OFFSET, card.vram_size, card.vram, 1));
    CHECK(mc_bart_map_device(&b, 0xC, MC_LIVADIX_CARD_REGS_OFFSET, 0x1000u, &card, &mc_livadix_card_ops));
    CHECK(mc_bart_map_memory(&b, 0xC, MC_LIVADIX_CARD_ROM_OFFSET, MC_LIVADIX_CARD_ROM_WINDOW, card.rom, 0));
    CHECK(!mc_bart_map_memory(&b, 0xC, MC_LIVADIX_CARD_VRAM_OFFSET, 16, card.vram, 1));   /* overlaps */
    /* The format block's last byte lane at the top of the slot: the byte
     * lanes value $0F/$E1/$D2/$B4/$78 (Designing Cards, chapter 8). */
    CHECK(mc_bart_read(&b, 0xFCFFFFFFu, 1, &v));
    CHECK((v & 0x0Fu) == ((~v >> 4) & 0x0Fu));
    CHECK(mc_bart_write(&b, 0xFC000010u, 4, 0x11223344u));
    CHECK_EQ(card.vram[0x10], 0x11);
    CHECK(mc_bart_read(&b, 0xFC000010u, 2, &v));
    CHECK_EQ(v, 0x1122);
    CHECK(!mc_bart_read(&b, 0xC0000000u, 4, &v));              /* super slot space: not decoded */
    CHECK(!mc_bart_read(&b, 0xFDFFFFFFu, 1, &v));
    CHECK_EQ(mc_bart_free_slot(&b), 0xD);
    mc_livadix_card_free(&card);
}

static void test_irq(void) {
    mc_bart b;
    mc_bart_init(&b, &fake.host, "BART", 0x7000);
    mc_bart_irq(&b, 0xD, 1);
    CHECK_EQ(hw_fake_value(&fake, &b, MC_BART_LINE_SLOTS), 0x2000);
    mc_bart_irq(&b, 0xE, 1);
    CHECK_EQ(hw_fake_value(&fake, &b, MC_BART_LINE_SLOTS), 0x6000);
    mc_bart_irq(&b, 0xD, 0);
    CHECK_EQ(hw_fake_value(&fake, &b, MC_BART_LINE_SLOTS), 0x4000);
}

int main(void) {
    hw_fake_init(&fake);
    test_registers();
    test_slots();
    test_irq();
    printf("test_bart: %d failure(s)\n", mc_test_failures);
    return mc_test_failures ? 1 : 0;
}
