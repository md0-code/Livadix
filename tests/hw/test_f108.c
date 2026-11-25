/* test_f108: the F108 (f108.h). Its memory controller: the bank layouts of
 * the 630 and 580 boards, the registers' 6-bit cycles with the +$7C latch
 * as the ROM drives them (ROM offsets $A0170 and $A05F4), the sizing
 * configuration (each bank at n x 64 MB, repeating through its window, empty
 * banks floating), the ROM's sizing walk and the packed configuration it
 * leaves. Its IDE interface: the task file at +4 x register with the byte-
 * swapped data register (an IDENTIFY DEVICE read through the window), no
 * second bus, the interrupt byte at +$101 (INTRQ in bit 5, the VBL in bit 6,
 * the enables of bits 2-3) and /SLOTIRQ. Against a fake host and a real
 * bus. */
#include "hw_test.h"
#include "asic/f108.h"
#include "storage/vdisk.h"

#include <stdlib.h>
#include <string.h>

static int tests;
static hw_fake fake;
static mc_bus bus;
static mc_f108 f108;
static uint8_t *ram;
static unsigned remaps;

#define MB (1u << 20)
#define WINDOW (64u * MB)

static void remap(void *ctx) {
    (void)ctx;
    remaps++;
    mc_f108_map(&f108, &bus, ram);
}

static uint32_t rd(uint32_t address) {
    uint32_t v = 0xDEADBEEFu;
    CHECK_EQ(mc_bus_read(&bus, address, 4, &v), 0);     /* RAM space never bus-errors */
    return v;
}
static void wr(uint32_t address, uint32_t value) { CHECK_EQ(mc_bus_write(&bus, address, 4, value), 0); }

/* A register write as the ROM makes it: the value rotated right 6 bits to
 * +$7C, then the value itself (ROM offset $A0170). */
static void reg_write(uint32_t offset, uint32_t value) {
    mc_f108_ops.write(&f108, 0x7C, 4, value >> 6 | value << 26);
    mc_f108_ops.write(&f108, offset, 4, value);
}
/* A register read as the ROM makes it: the register, then +$7C (ROM
 * offset $A05F4). */
static uint32_t reg_read(uint32_t offset) {
    const uint32_t low = mc_f108_ops.read(&f108, offset, 4);
    const uint32_t latch = mc_f108_ops.read(&f108, 0x7C, 4);
    return ((latch << 6 | latch >> 26) & ~0x3Fu) | low;
}

static void fresh(uint32_t mb, unsigned simm_banks) {
    uint32_t banks[MC_F108_BANKS];
    CHECK(mc_f108_layout(mb, 4, simm_banks, banks));
    free(ram);
    ram = calloc(1, (size_t)mb * MB);
    hw_fake_init(&fake);
    mc_f108_init(&f108, &fake.host, "F108", banks, remap, NULL);
    remaps = 0;
    mc_f108_map(&f108, &bus, ram);
}

/* The ROM's sizing configuration (ROM offset $A015E): the end at
 * $FFFFFFFF, bank n at n x 64 MB. */
static void sizing_configuration(void) {
    reg_write(0x2C, 0xFFFFFFFFu);
    for (unsigned n = 0; n < MC_F108_BANKS; n++) reg_write(0x04 + 4 * n, 0x40u * n);
}

/* A bank's size as the ROM's probe finds it: a pattern written at the top
 * longword of each 4 MB step from the window's top down, until one reads
 * back; then the lowest address it shows at. 0 for an empty bank. */
static uint32_t rom_size_probe(uint32_t base) {
    const uint32_t pattern = 0xAB96919Eu;
    for (uint32_t top = WINDOW; top >= 4u * MB; top -= 4u * MB) {
        const uint32_t a = base + top - 4u;
        const uint32_t saved = rd(a);
        wr(a, pattern);
        if (rd(a) == pattern) {
            uint32_t size = 4u * MB;
            while (size < top && rd(base + size - 4u) != pattern) size *= 2u;
            wr(a, saved);
            return size;
        }
    }
    return 0;
}

static void test_layout(void) {
    tests++;
    uint32_t b[MC_F108_BANKS];
    /* The 630: 4 MB on the board, one SIMM of one or two banks. */
    CHECK(mc_f108_layout(4, 4, 2, b));
    CHECK_EQ(b[0], 4 * MB); CHECK_EQ(b[1], 0);
    CHECK(mc_f108_layout(8, 4, 2, b));
    CHECK_EQ(b[1], 4 * MB); CHECK_EQ(b[2], 0);
    CHECK(mc_f108_layout(12, 4, 2, b));
    CHECK_EQ(b[1], 8 * MB);
    CHECK(mc_f108_layout(20, 4, 2, b));
    CHECK_EQ(b[1], 16 * MB);
    CHECK(mc_f108_layout(36, 4, 2, b));
    CHECK_EQ(b[1], 16 * MB); CHECK_EQ(b[2], 16 * MB); CHECK_EQ(b[3], 0);
    CHECK(!mc_f108_layout(52, 4, 2, b));
    /* The 580's two sockets. */
    CHECK(mc_f108_layout(52, 4, 4, b));
    CHECK_EQ(b[3], 16 * MB); CHECK_EQ(b[4], 0);
    CHECK(!mc_f108_layout(2, 4, 2, b));
}

static void test_registers(void) {
    tests++;
    fresh(36, 2);
    /* Until programmed: the banks packed from 0 in megabytes, the end the
     * last megabyte. */
    CHECK_EQ(reg_read(0x04), 0);
    CHECK_EQ(reg_read(0x08), 4);
    CHECK_EQ(reg_read(0x0C), 20);
    CHECK_EQ(reg_read(0x10), 36);
    CHECK_EQ(reg_read(0x2C), 35);
    /* A cycle carries 6 bits: the rest of a value comes from +$7C. */
    mc_f108_ops.write(&f108, 0x7C, 4, 0);
    mc_f108_ops.write(&f108, 0x30, 4, 0xA55A00DCu);
    CHECK_EQ(reg_read(0x30), 0x1C);
    reg_write(0x30, 0xA55A00DCu);
    reg_write(0x34, 0xA55A01E7u);
    CHECK_EQ(reg_read(0x30), 0xA55A00DCu);
    CHECK_EQ(reg_read(0x34), 0xA55A01E7u);
    /* A register read returns its low 6 bits and leaves the rest in +$7C. */
    CHECK_EQ(mc_f108_ops.read(&f108, 0x34, 4), 0x27);
    CHECK_EQ(mc_f108_ops.read(&f108, 0x7C, 4), 0x9E956807u);
    CHECK_EQ(remaps, 0);                    /* timing registers decode nothing */
    reg_write(0x08, 0x404u);                /* bit 10 kept, start 4 */
    CHECK_EQ(reg_read(0x08), 0x404u);
    CHECK_EQ(remaps, 1);                    /* a bank start remaps */
    reg_write(0x08, 0x404u);
    CHECK_EQ(remaps, 1);                    /* the same value does not */
}

static void test_sizing_windows(void) {
    tests++;
    fresh(36, 2);
    sizing_configuration();
    /* Bank 0 (4 MB) repeats through its 64 MB window. */
    wr(0x00000010u, 0x11111111u);
    CHECK_EQ(rd(0x00400010u), 0x11111111u);
    CHECK_EQ(rd(0x03C00010u), 0x11111111u);
    /* Bank 1 (16 MB) at 64 MB, every 16 MB. */
    wr(0x04000020u, 0x22222222u);
    CHECK_EQ(rd(0x05000020u), 0x22222222u);
    CHECK_EQ(rd(0x07000020u), 0x22222222u);
    CHECK_EQ(rd(0x00000020u), 0);
    CHECK_EQ(ram[4 * MB + 0x23], 0x22);
    /* Bank 2 (16 MB) at 128 MB. */
    wr(0x08000030u, 0x33333333u);
    CHECK_EQ(ram[20 * MB + 0x33], 0x33);
    /* An empty bank floats: reads 0, writes are lost. */
    wr(0x0C000040u, 0x44444444u);
    CHECK_EQ(rd(0x0C000040u), 0);
    CHECK_EQ(rd(0x3C000000u), 0);
    /* The ROM's probe sizes every bank. */
    CHECK_EQ(rom_size_probe(0x00000000u), 4 * MB);
    CHECK_EQ(rom_size_probe(0x04000000u), 16 * MB);
    CHECK_EQ(rom_size_probe(0x08000000u), 16 * MB);
    for (unsigned n = 3; n < MC_F108_BANKS; n++) CHECK_EQ(rom_size_probe(n * WINDOW), 0);
}

static void test_packed(void) {
    tests++;
    fresh(36, 2);
    sizing_configuration();
    for (unsigned n = 0; n < 3; n++) wr(n * WINDOW + 0x100u, 0xB0000000u + n);
    /* What the ROM leaves (ROM offset $A0604): each bank's start in
     * megabytes, the empty banks at the end of RAM, the last megabyte. */
    const uint32_t starts[MC_F108_BANKS] = {0, 4, 20, 36, 36};
    for (unsigned n = 0; n < MC_F108_BANKS; n++) reg_write(0x04 + 4 * n, starts[n]);
    reg_write(0x2C, 35);
    CHECK_EQ(rd(0x00000100u), 0xB0000000u);
    CHECK_EQ(rd(0x00400100u), 0xB0000001u);
    CHECK_EQ(rd(0x01400100u), 0xB0000002u);
    /* Contiguous: the last longword of one bank is followed by the next. */
    wr(0x013FFFFCu, 0x12345678u);
    CHECK_EQ(ram[20 * MB - 1], 0x78);
    wr(0x023FFFFCu, 0x9ABCDEF0u);
    CHECK_EQ(ram[36 * MB - 1], 0xF0);
    /* Above the end, nothing. */
    wr(0x02400000u, 0x55555555u);
    CHECK_EQ(rd(0x02400000u), 0);
    CHECK_EQ(rd(0x3FFFFFFCu), 0);
}

/* --- the IDE interface ---------------------------------------------------------- */

static uint32_t ide_rd(mc_f108_ide *i, uint32_t offset, unsigned size) { return mc_f108_ide_ops.read(i, offset, size); }
static void ide_wr(mc_f108_ide *i, uint32_t offset, unsigned size, uint32_t value) { mc_f108_ide_ops.write(i, offset, size, value); }

static mc_ata ata;
static mc_f108_ide ide;

/* The board's wire: INTRQ into the interrupt byte's bit 5. */
static void wire(hw_fake *f, void *dev, int line, int state) {
    (void)f;
    if (dev == &ata && line == MC_ATA_LINE_INTRQ) mc_f108_ide_set(&ide, MC_F108_INT_IDE, state);
}

static void test_ide(void) {
    tests++;
    hw_fake_init(&fake);
    fake.on_line = wire;
    mc_ata_init(&ata, &fake.host, 2);
    mc_f108_ide_init(&ide, &fake.host, "F108 IDE", &ata);
    /* A 1 MB disk as device 0 on the virtual disk. */
    const char *path = hw_work_path("test_f108", "disk.img");
    FILE *f = fopen(path, "wb");
    CHECK(f != NULL);
    if (!f) return;
    static uint8_t zero[1 << 20];
    fwrite(zero, 1, sizeof zero, f);
    fclose(f);
    char error[256];
    mc_disk *d = mc_disk_open(path, 0, 512, error, sizeof error);
    CHECK(d != NULL);
    if (!d) return;
    mc_vdisk *vd = mc_vdisk_create(&d, 1, &(mc_vdisk_drivers){zero, 512, NULL, 0}, error, sizeof error);
    CHECK(vd != NULL);
    mc_ata_attach_disk(&ata, 0, vd);
    mc_ata_reset(&ata);
    hw_fake_step(&fake, MC_ATA_T_RESET * 2);
    /* The task file: register r at +4r; status at +$1C, alternate status at +$38. */
    CHECK_EQ(ide_rd(&ide, 0x1C, 1) & 0xC0u, 0x40u);         /* DRDY, not BSY */
    CHECK_EQ(ide_rd(&ide, 0x38, 1), ide_rd(&ide, 0x1C, 1));
    ide_wr(&ide, 0x08, 1, 0x55);                            /* the sector count register */
    CHECK_EQ(ide_rd(&ide, 0x08, 1), 0x55);
    /* No second bus: its cycles float. */
    CHECK_EQ(ide_rd(&ide, 0x9C, 1), 0);
    /* IDENTIFY DEVICE with interrupts on: INTRQ shows in bit 5 of +$101
     * and pulls /SLOTIRQ. */
    ide_wr(&ide, 0x38, 1, 0x00);
    ide_wr(&ide, 0x18, 1, 0xA0);
    ide_wr(&ide, 0x1C, 1, 0xEC);
    hw_fake_step(&fake, MC_ATA_T_FIRST * 2);
    CHECK_EQ(ide_rd(&ide, 0x101, 1) & 0x20u, 0x20u);
    CHECK_EQ(ide_rd(&ide, 0x100, 2) & 0x20u, 0x20u);
    CHECK_EQ(hw_fake_line(&fake, &ide, MC_F108_LINE_SLOTIRQ), 1);
    CHECK_EQ(ide_rd(&ide, 0x1C, 1) & 0x08u, 0x08u);         /* DRQ; the status read clears INTRQ */
    CHECK_EQ(ide_rd(&ide, 0x101, 1) & 0x20u, 0);
    CHECK_EQ(hw_fake_line(&fake, &ide, MC_F108_LINE_SLOTIRQ), 0);
    /* The data register byte-swapped: the ATA word's low byte (DD0-7) in
     * bits 15..8 of what the 68040 reads. Word 49's LBA bit (bit 9) shows
     * as bit 1; word 0 of a fixed disk ($0040 or so, bit 15 clear) has its
     * high byte in bits 7..0. */
    uint16_t words[256];
    for (unsigned w = 0; w < 256; w++) words[w] = (uint16_t)ide_rd(&ide, 0x00, 2);
    CHECK_EQ(words[49] & 0x0002u, 0x0002u);
    CHECK_EQ(words[49] & 0x0200u, 0);
    CHECK_EQ(words[0] & 0x0080u, 0);
    CHECK(words[27] >> 8 >= 0x20 && words[27] >> 8 < 0x7F && (words[27] & 0xFFu) >= 0x20 && (words[27] & 0xFFu) < 0x7F);
    /* The VBL input in bit 6, always enabled. */
    mc_f108_ide_set(&ide, MC_F108_INT_VIDEO, 1);
    CHECK_EQ(ide_rd(&ide, 0x101, 1) & 0x40u, 0x40u);
    CHECK_EQ(hw_fake_line(&fake, &ide, MC_F108_LINE_SLOTIRQ), 1);
    mc_f108_ide_set(&ide, MC_F108_INT_VIDEO, 0);
    CHECK_EQ(hw_fake_line(&fake, &ide, MC_F108_LINE_SLOTIRQ), 0);
    /* Bits 2-3 count only when bits 0-1 enable them; bits 0-1 read back. */
    mc_f108_ide_set(&ide, 0x04, 1);
    CHECK_EQ(hw_fake_line(&fake, &ide, MC_F108_LINE_SLOTIRQ), 0);
    ide_wr(&ide, 0x101, 1, 0x01);
    CHECK_EQ(ide_rd(&ide, 0x101, 1) & 0x07u, 0x05u);
    CHECK_EQ(hw_fake_line(&fake, &ide, MC_F108_LINE_SLOTIRQ), 1);
    ide_wr(&ide, 0x101, 1, 0x0C);                           /* what the ROM writes at start: none enabled */
    CHECK_EQ(hw_fake_line(&fake, &ide, MC_F108_LINE_SLOTIRQ), 0);
    mc_ata_attach_disk(&ata, 0, NULL);
    mc_vdisk_destroy(vd);
    mc_disk_close(d);
}

int main(void) {
    CHECK(mc_bus_init(&bus));
    test_layout();
    test_registers();
    test_sizing_windows();
    test_packed();
    test_ide();
    mc_bus_free(&bus);
    free(ram);
    printf("test_f108: %d tests, %d failure(s)\n", tests, mc_test_failures);
    return mc_test_failures ? 1 : 0;
}
