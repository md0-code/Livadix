/* test_iop: Apple's I/O Processor (iop.h).
 *
 * Without arguments: the host interface (address register, auto-increment,
 * run/hold, the interrupts both ways), the IOP's timer, port and bypass
 * mode, with 65C02 programs assembled by hand from the opcode matrix.
 *
 * With a ROM image (the IIfx's or the Quadra 900/950's): the ROM's own IOP
 * kernels, taken from its 'iopc' resources at run time, downloaded the way
 * the ROM's IOP Manager does it and run on the model: the SCC kernel's
 * start in bypass mode, a byte through the SCC in bypass, its bypass
 * request (message 1, kind 4, as the ROM sends it) both ways; the SWIM
 * kernel's Initialize and DriveStatus (message 2, IOP SWIM Driver ERS).
 * Exits 77 when the image is missing or has no kernels. */
#include "hw_test.h"
#include "iop.h"
#include "scc.h"
#include "swim.h"
#include "disk.h"

#include <stdlib.h>

static hw_fake fake;
static mc_iop iop;
static mc_scc scc;
static mc_swim swim;

/* ---- The host side, with the IOP Manager's access widths ---- */

static uint32_t hread(uint32_t off, unsigned size) { return iop.dev.ops->read(&iop, off, size); }
static void hwrite(uint32_t off, unsigned size, uint32_t v) { iop.dev.ops->write(&iop, off, size, v); }
static void set_addr(uint16_t a) { hwrite(MC_IOP_ADDR_HI, 2, a); }       /* one word at +1 */
static uint8_t ctl(void) { return (uint8_t)hread(MC_IOP_CTL, 1); }
static void set_ctl(uint8_t v) { hwrite(MC_IOP_CTL, 1, v); }
static uint8_t peek(uint16_t a) { set_addr(a); return (uint8_t)hread(MC_IOP_DATA, 1); }
static void poke(uint16_t a, uint8_t v) { set_addr(a); hwrite(MC_IOP_DATA, 1, v); }
static int host_irq(void) { return hw_fake_line(&fake, &iop, MC_IOP_LINE_IRQ); }

/* Loads `code` at $0400 with the RESET vector there and IRQ at `irq`
 * through the host interface, and starts the 65C02. */
static void boot(const uint8_t *code, size_t n, uint16_t irq_at, const uint8_t *irq, size_t irq_n) {
    hw_fake_init(&fake);
    mc_iop_init(&iop, &fake.host, MC_IOP_SCC, &scc, &mc_scc_ops);
    mc_scc_init(&scc, mc_iop_chip_host(&iop));
    set_ctl(MC_IOP_INC);
    set_addr(0x0400);
    for (size_t i = 0; i < n; i++) hwrite(MC_IOP_DATA, 1, code[i]);
    set_addr(irq_at);
    for (size_t i = 0; i < irq_n; i++) hwrite(MC_IOP_DATA, 1, irq[i]);
    const uint8_t vectors[] = {0x00, 0x04, (uint8_t)irq_at, (uint8_t)(irq_at >> 8)};
    set_addr(0x7FFC);
    hwrite(MC_IOP_DATA, 4, (uint32_t)vectors[0] << 24 | (uint32_t)vectors[1] << 16 | (uint32_t)vectors[2] << 8 | vectors[3]);
    set_ctl(MC_IOP_INC | MC_IOP_RUN);
}

static void test_host_interface(void) {
    hw_fake_init(&fake);
    mc_iop_init(&iop, &fake.host, MC_IOP_SCC, &scc, &mc_scc_ops);
    mc_scc_init(&scc, mc_iop_chip_host(&iop));
    CHECK_EQ(ctl(), 0);
    /* The address register, written as one word at +1, read back. */
    set_addr(0x1234);
    CHECK_EQ(hread(MC_IOP_ADDR_HI, 2), 0x1234);
    CHECK_EQ(hread(0x00, 1), 0x12);
    CHECK_EQ(hread(0x03, 1), 0x34);
    /* Data without and with auto-increment; long accesses as four bytes. */
    hwrite(MC_IOP_DATA, 1, 0xA5);
    hwrite(MC_IOP_DATA, 1, 0x5A);
    CHECK_EQ(iop.ram[0x1234], 0x5A);
    set_ctl(MC_IOP_INC);
    CHECK_EQ(ctl(), MC_IOP_INC);
    set_addr(0x0100);
    hwrite(MC_IOP_DATA, 4, 0x01020304);
    CHECK_EQ(hread(MC_IOP_ADDR_HI, 2), 0x0104);
    set_addr(0x0100);
    CHECK_EQ(hread(MC_IOP_DATA + 2, 2), 0x0102);
    CHECK_EQ(hread(MC_IOP_DATA, 1), 0x03);
    /* A15 is not decoded on the RAM side. */
    poke(0xFFFF, 0x77);
    CHECK_EQ(iop.ram[0x7FFF], 0x77);
    /* Mirrors at +$10; nothing at +$0C. */
    CHECK_EQ(hread(0x14, 1), MC_IOP_INC);
    CHECK_EQ(hread(0x0C, 1), 0);
    /* Held: no time passes for the 65C02. */
    hw_fake_step(&fake, 100000);
    CHECK_EQ(iop.steps, 0);
    CHECK_EQ(hw_fake_deadline(&fake, &iop), MC_NEVER);
}

/* The 65C02 starts at its RESET vector, says it is alive in $031F, and
 * answers the host's interrupt with INT0. */
static void test_interrupts(void) {
    static const uint8_t code[] = {
        0xA2, 0xFF, 0x9A,               /* LDX #$FF; TXS */
        0xA9, 0xFF, 0x8D, 0x34, 0xF0,   /* LDA #$FF; STA $F034: clear the flags */
        0xA9, 0x10, 0x8D, 0x33, 0xF0,   /* LDA #$10; STA $F033: the host interrupt */
        0xA9, 0xFF, 0x8D, 0x1F, 0x03,   /* LDA #$FF; STA $031F */
        0x58,                           /* CLI */
        0xE6, 0x10, 0x80, 0xFC,         /* loop: INC $10; BRA loop */
    };
    static const uint8_t irq[] = {
        0x48,                           /* PHA */
        0xAD, 0x34, 0xF0, 0x85, 0x11,   /* LDA $F034; STA $11 */
        0xA9, 0x10, 0x8D, 0x34, 0xF0,   /* LDA #$10; STA $F034 */
        0xA9, 0x04, 0x8D, 0x35, 0xF0,   /* LDA #$04; STA $F035: INT0 */
        0xE6, 0x12,                     /* INC $12 */
        0x68, 0x40,                     /* PLA; RTI */
    };
    boot(code, sizeof code, 0x0420, irq, sizeof irq);
    CHECK_EQ(ctl(), MC_IOP_INC | MC_IOP_RUN);
    CHECK(hw_fake_deadline(&fake, &iop) != MC_NEVER);
    hw_fake_step(&fake, 20000);
    CHECK_EQ(peek(0x031F), 0xFF);
    CHECK(iop.ram[0x10] != 0);
    /* About C15M/8 cycles per second: 20000 C15M = 2500 cycles. */
    CHECK(iop.cpu.cycles >= 2490 && iop.cpu.cycles <= 2510);
    CHECK(!host_irq());
    set_ctl(MC_IOP_INC | MC_IOP_RUN | MC_IOP_GEN_INT);
    CHECK_EQ(ctl() & MC_IOP_GEN_INT, 0);
    hw_fake_step(&fake, 2000);
    CHECK_EQ(iop.ram[0x12], 1);
    CHECK_EQ(iop.ram[0x11], 0x10);
    CHECK(host_irq());
    CHECK_EQ(ctl(), MC_IOP_INC | MC_IOP_RUN | MC_IOP_INT0);
    set_ctl(MC_IOP_INC | MC_IOP_RUN | MC_IOP_INT0);         /* written 1: cleared */
    CHECK(!host_irq());
    CHECK_EQ(ctl(), MC_IOP_INC | MC_IOP_RUN);
    hw_fake_step(&fake, 2000);
    CHECK_EQ(iop.ram[0x12], 1);                             /* taken once */
    /* Held again: the loop stops; run restarts from RESET. */
    set_ctl(MC_IOP_INC);
    const uint8_t count = iop.ram[0x10];
    hw_fake_step(&fake, 2000);
    CHECK_EQ(iop.ram[0x10], count);
    iop.ram[0x031F] = 0;
    set_ctl(MC_IOP_INC | MC_IOP_RUN);
    hw_fake_step(&fake, 2000);
    CHECK_EQ(peek(0x031F), 0xFF);
}

/* The timer: loaded through $F011, reloaded from the latch, its flag in
 * $F034 bit 5. */
static void test_timer(void) {
    static const uint8_t code[] = {
        0xA2, 0xFF, 0x9A,
        0xA9, 0xFF, 0x8D, 0x34, 0xF0,   /* clear the flags */
        0xA9, 0x20, 0x8D, 0x33, 0xF0,   /* LDA #$20; STA $F033: the timer */
        0xA9, 0xFF, 0x8D, 0x12, 0xF0,   /* LDA #$FF; STA $F012: latch low */
        0xA9, 0x01, 0x8D, 0x11, 0xF0,   /* LDA #$01; STA $F011: latch high, load ($01FF) */
        0x58,                           /* CLI */
        0x80, 0xFE,                     /* BRA * */
    };
    static const uint8_t irq[] = {
        0x48, 0xE6, 0x12,               /* PHA; INC $12 */
        0xA9, 0x20, 0x8D, 0x34, 0xF0,   /* LDA #$20; STA $F034 */
        0x68, 0x40,
    };
    boot(code, sizeof code, 0x0420, irq, sizeof irq);
    /* Ten periods of $200 cycles after the load (about 25 cycles in). */
    hw_fake_step(&fake, (uint64_t)(25 + 10 * 0x200 + 100) * MC_IOP_CYCLE);
    CHECK_EQ(peek(0x12), 10);           /* the host access brings the IOP to now */
    CHECK_EQ(iop.latch, 0x01FF);
    CHECK(iop.timer <= 0x01FF);
}

/* Bypass: the host reaches the SCC at +$20 and gets its interrupt; out of
 * bypass the 65C02 has both, at $F040-$F043 and $F034 bit 3. */
static void test_bypass(void) {
    static const uint8_t code[] = {
        0xA2, 0xFF, 0x9A,
        0xA9, 0x0C, 0x8D, 0x41, 0xF0,   /* LDA #$0C; STA $F041: aCtl pointer 12 */
        0xA9, 0x5A, 0x8D, 0x41, 0xF0,   /* LDA #$5A; STA $F041: WR12A */
        0xA9, 0x0C, 0x8D, 0x40, 0xF0,   /* bCtl pointer 12 */
        0xA9, 0xA5, 0x8D, 0x40, 0xF0,   /* WR12B */
        0xA9, 0x01, 0x8D, 0x30, 0xF0,   /* LDA #$01; STA $F030: bypass */
        0xA5, 0x20, 0xF0, 0xFC,         /* wait: LDA $20; BEQ wait */
        0x9C, 0x30, 0xF0,               /* STZ $F030 */
        0xA9, 0x08, 0x8D, 0x33, 0xF0,   /* LDA #$08; STA $F033 */
        0xAD, 0x34, 0xF0, 0x85, 0x21,   /* LDA $F034; STA $21 */
        0x80, 0xFE,
    };
    boot(code, sizeof code, 0x0480, (const uint8_t[]){0x40}, 1);
    hw_fake_step(&fake, 2000);
    CHECK_EQ(ctl() & MC_IOP_BYPASS, MC_IOP_BYPASS);
    CHECK_EQ(scc.ch[MC_SCC_A].wr[12], 0x5A);
    CHECK_EQ(scc.ch[MC_SCC_B].wr[12], 0xA5);
    /* RR12 through the bypass window (aCtl +$22, bCtl +$20). */
    hwrite(MC_IOP_CHIP + 2, 1, 12);
    CHECK_EQ(hread(MC_IOP_CHIP + 2, 1), 0x5A);
    hwrite(MC_IOP_CHIP + 0, 1, 12);
    CHECK_EQ(hread(MC_IOP_CHIP + 0, 1), 0xA5);
    mc_iop_set_chip_irq(&iop, 1);
    CHECK(host_irq());
    CHECK_EQ(ctl() & MC_IOP_CHIP_INT, MC_IOP_CHIP_INT);
    iop.ram[0x20] = 1;                  /* leave bypass */
    hw_fake_step(&fake, 2000);
    CHECK_EQ(ctl() & (MC_IOP_BYPASS | MC_IOP_CHIP_INT), 0);
    CHECK(!host_irq());
    CHECK_EQ(iop.ram[0x21], 0x08);      /* the SCC's interrupt, now the IOP's */
    CHECK_EQ(hread(MC_IOP_CHIP + 2, 1), 0);
    mc_iop_set_chip_irq(&iop, 0);
}

static uint8_t port_out;
static unsigned port_changes;
static void port_output(void *ctx, uint8_t v, uint64_t t) { (void)ctx; (void)t; port_out = v; port_changes++; }
static uint8_t port_input(void *ctx, uint64_t t) { (void)ctx; (void)t; return (port_out & 2u) ? 0x04 : 0; }

static void test_port(void) {
    static const uint8_t code[] = {
        0xA2, 0xFF, 0x9A,
        0xA9, 0x02, 0x0C, 0x32, 0xF0,   /* LDA #$02; TSB $F032 */
        0xAD, 0x32, 0xF0, 0x85, 0x10,   /* LDA $F032; STA $10 */
        0xA9, 0x02, 0x1C, 0x32, 0xF0,   /* TRB $F032 */
        0xAD, 0x32, 0xF0, 0x85, 0x11,
        0x80, 0xFE,
    };
    boot(code, sizeof code, 0x0480, (const uint8_t[]){0x40}, 1);
    const mc_iop_port port = {NULL, port_output, port_input};
    mc_iop_set_port(&iop, &port);
    port_changes = 0;
    hw_fake_step(&fake, 2000);
    CHECK_EQ(iop.ram[0x10], 0x06);
    CHECK_EQ(iop.ram[0x11], 0x00);
    CHECK_EQ(port_changes, 2);
}

/* ---- Apple's kernels, from the ROM ---- */

static uint8_t *rom;
static size_t rom_size;

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

/* A resource of the ROM's resource list (the list's offset at $1A; each
 * entry: 8 bytes of model mask, the next entry's offset, the data's offset,
 * type, id, attributes, name; the data's size 8 bytes before it). */
static const uint8_t *rom_resource(const char *type, int16_t id, uint32_t *size) {
    if (rom_size < 0x40) return NULL;
    uint32_t entry = be32(rom + 0x1A);
    if (entry + 4 > rom_size) return NULL;
    entry = be32(rom + entry);
    for (unsigned guard = 0; entry && entry + 24 <= rom_size && guard < 4096; guard++) {
        const uint32_t next = be32(rom + entry + 8), data = be32(rom + entry + 12);
        if (!memcmp(rom + entry + 16, type, 4) && (int16_t)(rom[entry + 20] << 8 | rom[entry + 21]) == id &&
            data >= 8 && data <= rom_size) {
            *size = be32(rom + data - 8);
            if (data + *size > rom_size) return NULL;
            return rom + data;
        }
        entry = next;
    }
    return NULL;
}

/* The IOP Manager's download (observed in both ROMs): hold with
 * auto-increment, fill the 32 KiB with $FF and check it, clear it and check
 * it, write each [count, address, bytes] record, check them, clear $031F,
 * run, and wait for the kernel's $FF there. */
static int download(const uint8_t *image, uint32_t size) {
    set_ctl(MC_IOP_INC | MC_IOP_INT0 | MC_IOP_INT1);
    for (unsigned a = 0; a < MC_IOP_RAM_SIZE; a++) { set_addr((uint16_t)a); hwrite(MC_IOP_DATA, 1, 0xFF); }
    int ok = 1;
    for (unsigned a = 0; a < MC_IOP_RAM_SIZE && ok; a++) {
        set_addr((uint16_t)a);
        ok = hread(MC_IOP_DATA, 1) == 0xFF;
        set_addr((uint16_t)a);
        hwrite(MC_IOP_DATA, 1, 0);
    }
    for (unsigned a = 0; a < MC_IOP_RAM_SIZE && ok; a++) { set_addr((uint16_t)a); ok = hread(MC_IOP_DATA, 1) == 0; }
    CHECK(ok);
    uint32_t i = 0;
    while (i < size && image[i]) {
        const unsigned n = image[i];
        if (i + 3 + n > size) return 0;
        set_addr((uint16_t)(image[i + 1] << 8 | image[i + 2]));
        for (unsigned k = 0; k < n; k++) hwrite(MC_IOP_DATA, 1, image[i + 3 + k]);
        i += 3 + n;
    }
    i = 0;
    while (i < size && image[i]) {
        const unsigned n = image[i];
        set_addr((uint16_t)(image[i + 1] << 8 | image[i + 2]));
        for (unsigned k = 0; k < n; k++) ok &= hread(MC_IOP_DATA, 1) == image[i + 3 + k];
        i += 3 + n;
    }
    CHECK(ok);
    poke(0x031F, 0);
    set_ctl(MC_IOP_INC | MC_IOP_RUN | MC_IOP_INT0 | MC_IOP_INT1);
    for (unsigned t = 0; t < 2000; t++) {
        hw_fake_step(&fake, 1000);
        if (peek(0x031F) == 0xFF) return 1;
    }
    return 0;
}

static unsigned received[4], moves;
static uint8_t host_mem[0x10000];      /* the host's memory for data moves (addresses below $10000) */

/* IOP-to-host message 1, a data move (the ROM's handler passes the message
 * to its IOPMoveData, observed): kind (0 IOP to host, 1 host to IOP, 2
 * compare), the IOP number, a 16-bit count, the host's address, the IOP's
 * address. */
static void move_data(void) {
    uint8_t m[10];
    set_addr(0x0320);
    for (unsigned i = 0; i < sizeof m; i++) m[i] = (uint8_t)hread(MC_IOP_DATA, 1);
    const unsigned count = (unsigned)m[2] << 8 | m[3];
    const uint32_t host = be32(m + 4);
    const uint16_t at = (uint16_t)(m[8] << 8 | m[9]);
    set_addr(at);
    for (unsigned i = 0; i < count; i++) {
        uint8_t *h = &host_mem[(host + i) & 0xFFFFu];
        if (m[0] == 0) *h = (uint8_t)hread(MC_IOP_DATA, 1);
        else if (m[0] == 1) hwrite(MC_IOP_DATA, 1, *h);
    }
    moves++;
}

/* Messages the kernel posts (page $03): data moves done, the others left
 * as they are; then the IOP Manager's answer, state 3 and an interrupt. */
static void serve_iop_messages(void) {
    if (!(ctl() & MC_IOP_INT1)) return;
    set_ctl(MC_IOP_INC | MC_IOP_RUN | MC_IOP_INT1);
    const uint8_t max = peek(0x0300);
    for (unsigned n = 1; n <= max && n < 8; n++) {
        if (peek((uint16_t)(0x0300 + n)) != 1) continue;
        if (n < 4) received[n]++;
        if (n == 1) move_data();
        poke((uint16_t)(0x0300 + n), 3);
        set_ctl(MC_IOP_INC | MC_IOP_RUN | MC_IOP_GEN_INT);
    }
}

/* Sends host-to-IOP message `n` (as the IOP Manager: the bytes at
 * $0200 + n*$20, state 1, control $0E) and waits for INT0 and state 3;
 * the reply is read back into `reply`. */
static int message(unsigned n, const uint8_t *msg, unsigned len, uint8_t *reply, unsigned reply_len) {
    set_addr((uint16_t)(0x0200 + n * 0x20));
    for (unsigned i = 0; i < len; i++) hwrite(MC_IOP_DATA, 1, msg[i]);
    poke((uint16_t)(0x0200 + n), 1);
    set_ctl(MC_IOP_INC | MC_IOP_RUN | MC_IOP_GEN_INT);
    for (unsigned t = 0; t < 100000; t++) {
        hw_fake_step(&fake, 1000);
        serve_iop_messages();
        if (ctl() & MC_IOP_INT0) {
            set_ctl(MC_IOP_INC | MC_IOP_RUN | MC_IOP_INT0);
            if (peek((uint16_t)(0x0200 + n)) != 3) continue;
            set_addr((uint16_t)(0x0200 + n * 0x20));
            for (unsigned i = 0; i < reply_len; i++) reply[i] = (uint8_t)hread(MC_IOP_DATA, 1);
            poke((uint16_t)(0x0200 + n), 0);
            return 1;
        }
    }
    return 0;
}

static void opcode_report(const char *what) {
    static const uint8_t cmos[] = {0x80, 0xDA, 0x5A, 0xFA, 0x7A, 0x64, 0x74, 0x9C, 0x9E, 0x04, 0x0C, 0x14, 0x1C,
                                   0x12, 0x32, 0x52, 0x72, 0x92, 0xB2, 0xD2, 0xF2, 0x1A, 0x3A, 0x89, 0x34, 0x3C, 0x7C};
    unsigned used = 0, cmos_used = 0, bitops = 0;
    for (unsigned op = 0; op < 256; op++) {
        if (!(iop.cpu.used[op >> 5] & (1u << (op & 31)))) continue;
        used++;
        if ((op & 0x07) == 0x07) bitops++;
        for (size_t k = 0; k < sizeof cmos; k++) cmos_used += cmos[k] == op;
    }
    printf("%s: %llu instructions, %u opcodes (%u CMOS additions, %u Rockwell bit operations)\n", what,
           (unsigned long long)iop.steps, used, cmos_used, bitops);
}

static uint8_t sent[16];
static unsigned nsent;
static void on_tx(void *ctx, int ch, uint8_t b) { (void)ctx; if (ch == MC_SCC_B && nsent < 16) sent[nsent++] = b; }

static void scc_wreg(unsigned ctl_off, unsigned reg, uint8_t v) {
    hwrite(MC_IOP_CHIP + ctl_off, 1, (reg & 7) | (reg >= 8 ? 0x08 : 0));
    hwrite(MC_IOP_CHIP + ctl_off, 1, v);
}

static void kernel_scc(const uint8_t *image, uint32_t size) {
    hw_fake_init(&fake);
    mc_iop_init(&iop, &fake.host, MC_IOP_SCC, &scc, &mc_scc_ops);
    mc_scc_init(&scc, mc_iop_chip_host(&iop));
    const mc_scc_port port = {NULL, on_tx, NULL};
    mc_scc_set_port(&scc, &port);
    const int alive = download(image, size);
    CHECK(alive);
    if (!alive) return;
    CHECK_EQ(peek(0x0200), 7);
    CHECK_EQ(peek(0x0300), 7);
    /* The kernel starts in bypass: a byte out of channel B through the
       host's own SCC accesses at +$20 (async, x16, 8 bits, the BRG). */
    CHECK_EQ(ctl() & MC_IOP_BYPASS, MC_IOP_BYPASS);
    scc_wreg(0, 9, 0x40);               /* channel B reset */
    scc_wreg(0, 4, 0x44);
    scc_wreg(0, 3, 0xC1);
    scc_wreg(0, 5, 0xEA);
    scc_wreg(0, 11, 0x50);
    scc_wreg(0, 12, 0x0A);
    scc_wreg(0, 13, 0x00);
    scc_wreg(0, 14, 0x01);
    hwrite(MC_IOP_CHIP + 4, 1, 0x4D);   /* bData */
    hw_fake_step(&fake, 200000);
    CHECK_EQ(nsent, 1);
    CHECK_EQ(sent[0], 0x4D);
    /* And a byte in. */
    const uint8_t in = 0x6F;
    mc_scc_receive(&scc, MC_SCC_B, &in, 1);
    hw_fake_step(&fake, 200000);
    hwrite(MC_IOP_CHIP + 0, 1, 0);
    CHECK_EQ(hread(MC_IOP_CHIP + 0, 1) & 1, 1);       /* RR0: Rx character available */
    CHECK_EQ(hread(MC_IOP_CHIP + 4, 1), 0x6F);
    /* The ROM's request: message 1, kind 4, 0 = leave bypass, owner $FF. */
    uint8_t reply[4] = {0xEE, 0xEE, 0xEE, 0xEE};
    const uint8_t off[] = {0x04, 0x00, 0xFF};
    CHECK(message(1, off, sizeof off, reply, 3));
    CHECK_EQ(reply[0], 0);
    CHECK_EQ(ctl() & MC_IOP_BYPASS, 0);
    CHECK_EQ(hread(MC_IOP_CHIP + 0, 1), 0);           /* the chip is the IOP's now */
    /* The kernel reset both channels and set them up (its table). */
    CHECK_EQ(scc.ch[MC_SCC_A].wr[4], 0x4C);
    CHECK_EQ(scc.ch[MC_SCC_B].wr[4], 0x4C);
    CHECK_EQ(scc.ch[MC_SCC_A].wr[3], 0xC0);
    CHECK_EQ(iop.ie & 0x08, 0x08);                    /* the SCC's interrupt enabled on the IOP */
    /* Back into bypass for owner 1. */
    const uint8_t on[] = {0x04, 0x01, 0x01};
    CHECK(message(1, on, sizeof on, reply, 3));
    CHECK_EQ(reply[0], 0);
    CHECK_EQ(ctl() & MC_IOP_BYPASS, MC_IOP_BYPASS);
    /* A second owner is refused with the current owner (-4: $FC). */
    const uint8_t again[] = {0x04, 0x01, 0x02};
    CHECK(message(1, again, sizeof again, reply, 3));
    CHECK_EQ(reply[0], 0xFC);
    CHECK_EQ(reply[1], 0x01);
    opcode_report("SCC kernel");
}

static uint8_t adb_low;
static void adb_output(void *ctx, uint8_t v, uint64_t t) { (void)ctx; (void)t; adb_low = (v & 2u) != 0; }
static uint8_t adb_input(void *ctx, uint64_t t) { (void)ctx; (void)t; return adb_low ? 0x04 : 0; }

static void kernel_swim(const uint8_t *image, uint32_t size) {
    hw_fake_init(&fake);
    mc_iop_init(&iop, &fake.host, MC_IOP_SWIM, &swim, &mc_swim_ops);
    mc_swim_init(&swim, mc_iop_chip_host(&iop), 2);
    const mc_iop_port port = {NULL, adb_output, adb_input};
    mc_iop_set_port(&iop, &port);
    /* A blank 1440K disk in the internal drive; a file per ROM image (named
     * by a hash of its kernel), as ctest runs both ROMs' kernels at once. */
    uint32_t hash = 2166136261u;
    for (uint32_t i = 0; i < size; i++) hash = (hash ^ image[i]) * 16777619u;
    char name[40];
    snprintf(name, sizeof name, "blank1440-%08X.img", (unsigned)hash);
    const char *path = hw_work_path("iop", name);
    FILE *f = fopen(path, "wb");
    CHECK(f != NULL);
    if (!f) return;
    static uint8_t image_data[1474560];
    for (uint32_t i = 0; i < sizeof image_data; i++) image_data[i] = (uint8_t)(i * 13u ^ i >> 9);
    fwrite(image_data, 1, sizeof image_data, f);
    fclose(f);
    char error[200];
    mc_disk *disk = mc_disk_open(path, 1, 512, error, sizeof error);
    CHECK(disk != NULL);
    if (!disk) return;
    CHECK(mc_swim_insert(&swim, 0, disk, 0, error, sizeof error));
    const int alive = download(image, size);
    CHECK(alive);
    if (alive) {
        CHECK_EQ(peek(0x0200), 3);
        CHECK_EQ(peek(0x0300), 3);
        /* Initialize: error 0 and the drive kinds (4 = 400K/800K/720K/1440K). */
        uint8_t reply[32];
        const uint8_t init[] = {0x01, 0x00};
        CHECK(message(2, init, sizeof init, reply, sizeof reply));
        CHECK_EQ(reply[0], 0x01);
        CHECK_EQ(reply[2] << 8 | reply[3], 0);
        unsigned fdhd = 0, first = 0;
        for (unsigned d = 0; d < 28; d++) {
            if (reply[4 + d] == 4 && !fdhd++) first = d;
        }
        CHECK_EQ(fdhd, 2);
        printf("SWIM kernel: drive kinds");
        for (unsigned d = 0; d < 28; d++) printf(" %u", reply[4 + d]);
        printf("\n");
        /* StartPolling: the kernel finds the disk and posts DiskInserted
           (IOP-to-host message 2). */
        const uint8_t poll[] = {0x03, 0x00};
        CHECK(message(2, poll, sizeof poll, reply, 4));
        CHECK_EQ(reply[2] << 8 | reply[3], 0);
        received[2] = 0;
        for (unsigned t = 0; t < 60000 && !received[2]; t++) {
            hw_fake_step(&fake, 1000);
            serve_iop_messages();
        }
        printf("SWIM kernel: DiskInserted by %.3f s (%u)\n", (double)fake.now / MC_C15M_HZ, received[2]);
        CHECK_EQ(received[2], 1);
        /* DriveStatus of the first SuperDrive: installed, the disk in place,
           an MFM drive. */
        const uint8_t status[] = {0x06, (uint8_t)first};
        CHECK(message(2, status, sizeof status, reply, sizeof reply));
        CHECK_EQ(reply[2] << 8 | reply[3], 0);
        printf("SWIM kernel: drive %u status", first);
        for (unsigned i = 0; i < 32; i++) printf(" %02X", reply[i]);
        printf("\n");
        CHECK(reply[0x07] != 0);        /* Disk In Place */
        CHECK(reply[0x08] != 0);        /* Installed */
        CHECK(reply[0x12] != 0);        /* MFM Drive */
        /* Read: blocks 0-1 and block 1000 into host memory, through the
           kernel's data-move requests. */
        static const struct { uint32_t block, count; } reads[] = {{0, 2}, {1000, 1}};
        for (unsigned r = 0; r < 2; r++) {
            memset(host_mem, 0xEE, sizeof host_mem);
            const uint32_t block = reads[r].block, count = reads[r].count;
            const uint8_t rd[16] = {0x0B, (uint8_t)first, 0, 0, 0x00, 0x00, 0x10, 0x00,
                                    (uint8_t)(block >> 24), (uint8_t)(block >> 16), (uint8_t)(block >> 8), (uint8_t)block,
                                    (uint8_t)(count >> 24), (uint8_t)(count >> 16), (uint8_t)(count >> 8), (uint8_t)count};
            moves = 0;
            CHECK(message(2, rd, sizeof rd, reply, 4));
            CHECK_EQ(reply[2] << 8 | reply[3], 0);
            CHECK(moves > 0);
            CHECK(!memcmp(host_mem + 0x1000, image_data + block * 512u, count * 512u));
            CHECK_EQ(host_mem[0x1000 + count * 512u], 0xEE);
        }
        /* Write: two blocks from host memory, then read back through the
           kernel and from the image. */
        uint8_t pattern[1024];
        for (unsigned i = 0; i < sizeof pattern; i++) pattern[i] = (uint8_t)(i * 7u + 3u);
        memcpy(host_mem + 0x2000, pattern, sizeof pattern);
        const uint8_t wr[16] = {0x0A, (uint8_t)first, 0, 0, 0x00, 0x00, 0x20, 0x00, 0, 0, 0x01, 0x2C, 0, 0, 0, 2};
        moves = 0;
        CHECK(message(2, wr, sizeof wr, reply, 4));
        CHECK_EQ(reply[2] << 8 | reply[3], 0);
        CHECK(moves > 0);
        memset(host_mem, 0xEE, sizeof host_mem);
        const uint8_t back[16] = {0x0B, (uint8_t)first, 0, 0, 0x00, 0x00, 0x10, 0x00, 0, 0, 0x01, 0x2C, 0, 0, 0, 2};
        CHECK(message(2, back, sizeof back, reply, 4));
        CHECK_EQ(reply[2] << 8 | reply[3], 0);
        CHECK(!memcmp(host_mem + 0x1000, pattern, sizeof pattern));
        mc_swim_flush(&swim);
        uint8_t on_disk[1024];
        CHECK(mc_disk_read(disk, 300, 2, on_disk));
        CHECK(!memcmp(on_disk, pattern, sizeof pattern));
        printf("SWIM kernel: blocks read and written by %.3f s of guest time\n", (double)fake.now / MC_C15M_HZ);
        opcode_report("SWIM kernel");
    }
    mc_swim_remove(&swim, 0);
    mc_disk_close(disk);
}

static int run_kernels(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { printf("test_iop: no %s, skipped\n", path); return 77; }
    fseek(f, 0, SEEK_END);
    rom_size = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    rom = malloc(rom_size);
    if (!rom || fread(rom, 1, rom_size, f) != rom_size) { fclose(f); return 77; }
    fclose(f);
    uint32_t scc_size = 0, swim_size = 0;
    const uint8_t *scc_image = rom_resource("iopc", 0, &scc_size);
    const uint8_t *swim_image = rom_resource("iopc", 1, &swim_size);
    if (!scc_image || !swim_image) { printf("test_iop: %s has no IOP kernels, skipped\n", path); free(rom); return 77; }
    kernel_scc(scc_image, scc_size);
    kernel_swim(swim_image, swim_size);
    free(rom);
    printf("test_iop %s: %d failure(s)\n", path, mc_test_failures);
    return mc_test_failures ? 1 : 0;
}

int main(int argc, char **argv) {
    if (argc > 1) return run_kernels(argv[1]);
    test_host_interface();
    test_interrupts();
    test_timer();
    test_bypass();
    test_port();
    printf("test_iop: %d failure(s)\n", mc_test_failures);
    return mc_test_failures ? 1 : 0;
}
