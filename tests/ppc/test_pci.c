/* The first PCI Power Macintosh generation's chips (phase 6.8),
 * against their sources: the PCI Local Bus
 * Specification 2.0 chapter 6 (configuration space), Designing PCI Cards
 * and Drivers for Power Macintosh Computers (the bridges' address
 * invariance and control spaces), the CHRP I/O Device Reference chapters
 * 12 (MESH) and 15 (DBDMA), the Developer Notes, and ROM 9630C68B's own
 * use (POST, Open Firmware's FCode). `test_pci probe [ROM]` runs a PCI
 * ROM's POST on a test board of these chips (not a machine description)
 * through its memory sizing, which it checks in the NVRAM the ROM writes.
 *
 * Livadix's own code. */
#include "ppc/ppc_machine.h"
#include "ppc/chips/awacs.h"
#include "ppc/chips/bandit.h"
#include "ppc/chips/control.h"
#include "ppc/chips/dbdma.h"
#include "ppc/chips/grand_central.h"
#include "ppc/chips/hammerhead.h"
#include "ppc/chips/mesh.h"
#include "ppc/chips/nvram.h"
#include "ppc/chips/pci.h"
#include "ppc/chips/planb.h"
#include "ppc/chips/platinum.h"
#include "ppc/chips/radacal.h"
#include "via.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned assertions, failures;
#define CHECK(x) do { ++assertions; if (!(x)) { ++failures; fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); } } while (0)

/* --- a host: guest time, deadlines, lines, 1 MB of memory ------------------------ */

#define MEM (1u << 20)
static uint8_t mem[MEM];
static uint64_t now;
typedef struct deadline { void *dev; uint64_t when; } deadline;
static deadline due[16];
static int lines[8][4];             /* per test device slot, per line: the state */
static unsigned rises[8][4];
static void *slot_dev[8];
static mc_dbdma *mesh_dma_channel;

static uint64_t h_now(void *ctx) { (void)ctx; return now; }
static void h_schedule(void *ctx, void *dev, uint64_t when) {
    (void)ctx;
    for (int i = 0; i < 16; i++) if (due[i].dev == dev) { due[i].when = when; return; }
    for (int i = 0; i < 16; i++) if (!due[i].dev) { due[i] = (deadline){dev, when}; return; }
}
static void h_line(void *ctx, void *dev, int line, int state) {
    (void)ctx;
    if (mesh_dma_channel && dev == slot_dev[1] && line == MC_MESH_LINE_DMA_STATUS)
        mc_dbdma_set_bits(mesh_dma_channel, 0xE0, (uint8_t)state);
    for (int i = 0; i < 8; i++)
        if (slot_dev[i] == dev && line < 4) { if (state && !lines[i][line]) rises[i][line]++; lines[i][line] = state; }
}
static int h_dma(void *ctx, uint32_t address, void *data, uint32_t length, int write) {
    (void)ctx;
    if ((uint64_t)address + length > MEM) return 0;
    if (write) memcpy(mem + address, data, length);
    else memcpy(data, mem + address, length);
    return 1;
}
static const mc_hw_host host = {NULL, h_now, h_schedule, h_line, h_dma};

static void fresh(void) {
    now = 0;
    memset(due, 0, sizeof due);
    memset(lines, 0, sizeof lines);
    memset(rises, 0, sizeof rises);
    memset(slot_dev, 0, sizeof slot_dev);
    memset(mem, 0, sizeof mem);
}
/* Guest time to `t`, deadlines firing in order. */
static void advance(uint64_t t) {
    for (;;) {
        int k = -1;
        for (int i = 0; i < 16; i++)
            if (due[i].dev && due[i].when != MC_NEVER && due[i].when <= t && (k < 0 || due[i].when < due[k].when)) k = i;
        if (k < 0) break;
        now = due[k].when;
        due[k].when = MC_NEVER;
        ((mc_dev *)due[k].dev)->ops->event(due[k].dev, now);
    }
    now = t;
}
static void put32le(uint32_t a, uint32_t v) { mem[a] = (uint8_t)v; mem[a + 1] = (uint8_t)(v >> 8); mem[a + 2] = (uint8_t)(v >> 16); mem[a + 3] = (uint8_t)(v >> 24); }
static uint32_t get32le(uint32_t a) { return mem[a] | (uint32_t)mem[a + 1] << 8 | (uint32_t)mem[a + 2] << 16 | (uint32_t)mem[a + 3] << 24; }
/* A DBDMA command (Table 333). */
static void command(uint32_t at, unsigned cmd, unsigned key, unsigned i, unsigned b, unsigned w, unsigned req, uint32_t address, uint32_t dep) {
    put32le(at, (uint32_t)cmd << 28 | (uint32_t)key << 24 | (uint32_t)i << 20 | (uint32_t)b << 18 | (uint32_t)w << 16 | req);
    put32le(at + 4, address);
    put32le(at + 8, dep);
    put32le(at + 12, 0);
}

/* --- PCI configuration space (PCI 2.0, 6.2) ----------------------------------------- */

static void pci_config(void) {
    mc_pci_config c;
    mc_pci_config_init(&c, 0x106B, 0x0002, 0x01, 0xFF0000, 0);
    mc_pci_config_bar(&c, 0, 0x20000, 0);
    CHECK(mc_pci_config_read(&c, 0, 4) == 0x0002106Bu);
    CHECK(mc_pci_config_read(&c, 8, 4) == 0xFF000001u);
    mc_pci_config_write(&c, 0, 4, 0xFFFFFFFFu);                 /* IDs read only */
    CHECK(mc_pci_config_read(&c, 0, 2) == 0x106B);
    mc_pci_config_write(&c, MC_PCI_BAR0, 4, 0xFFFFFFFFu);      /* 6.2.5.1: sizing */
    CHECK(mc_pci_config_read(&c, MC_PCI_BAR0, 4) == 0xFFFE0000u);
    mc_pci_config_write(&c, MC_PCI_BAR0, 4, 0xF3000000u);
    CHECK(mc_pci_config_base(&c, 0) == 0xF3000000u);
    CHECK(!mc_pci_config_hit(&c, 0, 0xF3000010u, NULL));        /* memory space disabled */
    mc_pci_config_write(&c, MC_PCI_COMMAND, 2, 0x0016);
    CHECK(mc_pci_config_read(&c, MC_PCI_COMMAND, 2) == 0x0016);
    uint32_t off = 0;
    CHECK(mc_pci_config_hit(&c, 0, 0xF3018030u, &off) && off == 0x18030u);
    CHECK(!mc_pci_config_hit(&c, 0, 0xF3020000u, NULL));
    c.reg[MC_PCI_STATUS + 1] = 0x30;                            /* 6.2.3: status bits clear by writing 1 */
    mc_pci_config_write(&c, MC_PCI_STATUS, 2, 0x2000);
    CHECK(c.reg[MC_PCI_STATUS + 1] == 0x10);
    mc_pci_config_bar(&c, 1, 0x100, 1);
    mc_pci_config_write(&c, MC_PCI_BAR0 + 4, 4, 0xFFFFFFFFu);
    CHECK(mc_pci_config_read(&c, MC_PCI_BAR0 + 4, 4) == 0xFFFFFF01u);
}

/* --- Bandit (configuration mechanism, windows, byte lanes) ----------------------- */

static void bandit(void) {
    fresh();
    mc_bandit b;
    mc_gc g;
    mc_bandit_init(&b, &host, "bandit", MC_BANDIT_BANDIT, 0xF2000000u);
    mc_gc_init(&g, &host, "gc");
    const mc_pci_function gf = mc_gc_function(&g);
    CHECK(mc_bandit_attach(&b, MC_GC_DEVICE, &gf));
    CHECK(!mc_bandit_attach(&b, MC_BANDIT_SELF, &gf));
    const mc_dev_ops *o = &mc_bandit_ops;
    /* POST's sequence ($FFF03688): the address register takes stwbrx
     * values, the bytes reach PCI by address. */
    o->write(&b, MC_BANDIT_CONFIG_ADDRESS, 4, 0x00080000u);    /* bytes 00 08 00 00: IDSEL AD11, register 0 */
    CHECK(b.config_address == 0x00000800u);
    CHECK(o->read(&b, MC_BANDIT_CONFIG_ADDRESS, 4) == 0x00080000u);
    CHECK(o->read(&b, MC_BANDIT_CONFIG_DATA, 4) == 0x6B100100u);    /* vendor $106B, device $0001 */
    CHECK(o->read(&b, MC_BANDIT_CONFIG_DATA + 2, 2) == 0x0100u);
    o->write(&b, MC_BANDIT_CONFIG_ADDRESS, 4, 0x08080000u);    /* register 8: revision and class */
    CHECK(o->read(&b, MC_BANDIT_CONFIG_DATA, 4) == 0x03000006u);  /* revision 3†, class $060000 */
    o->write(&b, MC_BANDIT_CONFIG_ADDRESS, 4, 0x10000100u);    /* GC: IDSEL AD16, BAR0 */
    o->write(&b, MC_BANDIT_CONFIG_DATA, 4, 0x000000F3u);       /* $F300 0000 */
    o->write(&b, MC_BANDIT_CONFIG_ADDRESS, 4, 0x04000100u);
    o->write(&b, MC_BANDIT_CONFIG_DATA, 4, 0x16000000u);       /* command $16 */
    CHECK(mc_pci_config_base(&g.cfg, 0) == 0xF3000000u);
    CHECK(g.cfg.reg[MC_PCI_COMMAND] == 0x16);
    o->write(&b, MC_BANDIT_CONFIG_ADDRESS, 4, 0x00000100u);    /* GC's IDs */
    CHECK(o->read(&b, MC_BANDIT_CONFIG_DATA, 4) == 0x6B100200u);
    o->write(&b, MC_BANDIT_CONFIG_ADDRESS, 4, 0x00200000u);    /* IDSEL AD13: nobody */
    CHECK(o->read(&b, MC_BANDIT_CONFIG_DATA, 4) == 0xFFFFFFFFu);
    o->write(&b, MC_BANDIT_CONFIG_ADDRESS, 4, 0x01000100u);    /* type 1 (bit 0): no bus behind */
    CHECK(o->read(&b, MC_BANDIT_CONFIG_DATA, 4) == 0xFFFFFFFFu);
    CHECK(mc_pci_config_read(&b.cfg, MC_PCI_STATUS, 2) & MC_PCI_ST_MASTER_ABORT);
    /* The memory window (control space + $100 0000): GC's mask register,
     * a word written $80000000 as POST does it with stwbrx. */
    o->write(&b, 0x01000024u, 4, 0x00000080u);
    CHECK(g.mask == 0x80000000u);
    CHECK(o->read(&b, 0x01000024u, 4) == 0x00000080u);
    CHECK(o->read(&b, 0x01000025u, 1) == 0x00);
    CHECK(o->read(&b, 0x01000027u, 1) == 0x80);
    uint32_t v;
    CHECK(!mc_bandit_mem_read(&b, 0xF3100000u, 4, &v) && v == 0xFFFFFFFFu);  /* past GC's 1 MB */
    /* The address mask (register $48, the bandit FCode's map-range). */
    CHECK(mc_bandit_decodes(&b, 0xF3000000u));
    CHECK(!mc_bandit_decodes(&b, 0x80000000u));
    mc_bandit_config_write(&b, MC_BANDIT_SELF, 0, MC_BANDIT_ADDRESS_MASK, 4, (1u << (16 + 8)) | (1u << 0xA));
    CHECK(mc_bandit_decodes(&b, 0x80000000u) && mc_bandit_decodes(&b, 0x8FFFFFFFu));
    CHECK(!mc_bandit_decodes(&b, 0x90000000u));
    CHECK(mc_bandit_decodes(&b, 0xFA000000u) && !mc_bandit_decodes(&b, 0xFB000000u));
    CHECK(mc_bandit_config_read(&b, MC_BANDIT_SELF, 0, MC_BANDIT_MODE, 1) == 1);
    mc_bandit_config_write(&b, MC_BANDIT_SELF, 0, MC_BANDIT_MODE, 1, 0);    /* OF: big-endian */
    CHECK(mc_bandit_config_read(&b, MC_BANDIT_SELF, 0, MC_BANDIT_MODE, 1) == 0);
    /* I/O space: no function decodes it. */
    CHECK(o->read(&b, 0x00000400u, 2) == 0xFFFFu);

    /* Chaos: the same mechanism, Control at device 11, Plan B at 13. */
    mc_bandit ch;
    mc_control_chip c;
    mc_planb pb;
    mc_bandit_init(&ch, &host, "chaos", MC_BANDIT_CHAOS, 0xF0000000u);
    CHECK(mc_control_init(&c, &host, "control", 4u << 20));
    mc_planb_init(&pb, &host, "planb");
    const mc_pci_function cf = mc_control_function(&c), pf = mc_planb_function(&pb);
    CHECK(mc_bandit_attach(&ch, MC_CONTROL_DEVICE, &cf));
    CHECK(mc_bandit_attach(&ch, MC_PLANB_DEVICE, &pf));
    mc_bandit_ops.write(&ch, MC_BANDIT_CONFIG_ADDRESS, 4, 0x00080000u);
    CHECK(mc_bandit_ops.read(&ch, MC_BANDIT_CONFIG_DATA, 4) == 0x6B100300u);
    mc_bandit_ops.write(&ch, MC_BANDIT_CONFIG_ADDRESS, 4, 0x00200000u);
    CHECK(mc_bandit_ops.read(&ch, MC_BANDIT_CONFIG_DATA, 4) == 0x6B100400u);
    /* The chaos node's set-up (file $3463C2): Control's $10 = 2, $14 = $9000 0000. */
    mc_bandit_ops.write(&ch, MC_BANDIT_CONFIG_ADDRESS, 4, 0x10080000u);
    mc_bandit_ops.write(&ch, MC_BANDIT_CONFIG_DATA, 4, 0x00000002u);
    mc_bandit_ops.write(&ch, MC_BANDIT_CONFIG_ADDRESS, 4, 0x14080000u);
    mc_bandit_ops.write(&ch, MC_BANDIT_CONFIG_DATA, 4, 0x00000090u);
    CHECK(mc_pci_config_read(&c.cfg, 0x10, 4) == 0x02000000u);
    CHECK(mc_pci_config_base(&c.cfg, 1) == 0x90000000u);
    mc_control_free(&c);
}

/* --- DBDMA (CHRP I/O 15) ------------------------------------------------------------- */

typedef struct port { uint8_t out[64]; unsigned n, room; uint8_t in[64]; unsigned in_n, in_pos, ends; int end; } port;
static int port_write(void *p, uint8_t b) { port *q = p; if (q->n >= q->room) return 0; q->out[q->n++] = b; return 1; }
static int port_read(void *p, uint8_t *b) {
    port *q = p;
    if (q->in_pos < q->in_n) { *b = q->in[q->in_pos++]; return MC_DBDMA_BYTE; }
    return q->end ? MC_DBDMA_END : MC_DBDMA_NONE;
}
static void port_end(void *p) { ((port *)p)->ends++; }
static unsigned irqs;
static void dma_irq(void *ctx, unsigned channel) { (void)ctx; (void)channel; irqs++; }
static int dma_mem(void *ctx, uint32_t a, void *d, uint32_t l, int w) { return h_dma(ctx, a, d, l, w); }

static void dbdma(void) {
    fresh();
    port pt = {.room = 64};
    const mc_dbdma_host dh = {NULL, dma_mem, dma_irq};
    const mc_dbdma_port dp = {&pt, port_read, port_write, NULL};
    mc_dbdma ch;
    mc_dbdma_init(&ch, &dh, 8, &dp);
    irqs = 0;
    CHECK(mc_dbdma_read(&ch, MC_DBDMA_STATUS, 4) == 0);
    /* OUTPUT_MORE 4 bytes, OUTPUT_LAST 3 with an interrupt, STOP. */
    memcpy(mem + 0x100, "ABCDEFG", 7);
    command(0x1000, MC_DBDMA_OUTPUT_MORE, 0, 0, 0, 0, 4, 0x100, 0);
    command(0x1010, MC_DBDMA_OUTPUT_LAST, 0, 3, 0, 0, 3, 0x104, 0);
    command(0x1020, MC_DBDMA_STOP, 0, 0, 0, 0, 0, 0, 0);
    mc_dbdma_write(&ch, MC_DBDMA_COMMAND, 4, 0x1000);
    CHECK(mc_dbdma_read(&ch, MC_DBDMA_COMMAND, 4) == 0x1000);
    CHECK(mc_dbdma_write(&ch, MC_DBDMA_CONTROL, 4, 0x80008000u) == 1);      /* run */
    CHECK(mc_dbdma_read(&ch, MC_DBDMA_STATUS, 4) & MC_DBDMA_ACTIVE);
    mc_dbdma_write(&ch, MC_DBDMA_COMMAND, 4, 0x2000);                       /* ignored while running */
    CHECK(ch.command == 0x1000);
    CHECK(mc_dbdma_run(&ch, 16) == 0);
    CHECK(pt.n == 7 && !memcmp(pt.out, "ABCDEFG", 7));
    CHECK(irqs == 1);
    CHECK((get32le(0x100C) >> 16) == (MC_DBDMA_RUN | MC_DBDMA_ACTIVE) && (get32le(0x100C) & 0xFFFF) == 0);
    CHECK(ch.command == 0x1020);                                             /* STOP keeps it */
    CHECK(!(mc_dbdma_read(&ch, MC_DBDMA_STATUS, 4) & MC_DBDMA_ACTIVE));
    /* Append and wake (15.7.6). */
    command(0x1020, MC_DBDMA_STORE_QUAD, MC_DBDMA_KEY_SYSTEM, 0, 0, 0, 4, 0x200, 0xCAFEF00Du);
    command(0x1030, MC_DBDMA_STORE_QUAD, MC_DBDMA_KEY_SYSTEM, 0, 0, 0, 2, 0x206, 0x12345678u);
    command(0x1040, MC_DBDMA_LOAD_QUAD, MC_DBDMA_KEY_SYSTEM, 0, 0, 0, 4, 0x200, 0);
    command(0x1050, MC_DBDMA_STOP, 0, 0, 0, 0, 0, 0, 0);
    CHECK(mc_dbdma_write(&ch, MC_DBDMA_CONTROL, 4, 0x10001000u) == 1);      /* wake */
    mc_dbdma_run(&ch, 16);
    CHECK(get32le(0x200) == 0xCAFEF00Du);
    CHECK(mem[0x206] == 0x78 && mem[0x207] == 0x56);                          /* the low bytes (15.7.3) */
    CHECK(get32le(0x1048) == 0xCAFEF00Du);                                   /* LOAD_QUAD's data32 */
    CHECK(ch.command == 0x1050);
    /* A stalling port, then a kick (the device's request). */
    mc_dbdma_write(&ch, MC_DBDMA_CONTROL, 4, 0x80000000u);                  /* run off */
    pt.n = 0;
    pt.room = 2;
    command(0x1100, MC_DBDMA_OUTPUT_LAST, 0, 0, 0, 0, 5, 0x100, 0);
    command(0x1110, MC_DBDMA_STOP, 0, 0, 0, 0, 0, 0, 0);
    mc_dbdma_write(&ch, MC_DBDMA_COMMAND, 4, 0x1100);
    mc_dbdma_write(&ch, MC_DBDMA_CONTROL, 4, 0x80008000u);
    CHECK(mc_dbdma_run(&ch, 16) == 0 && pt.n == 2 && ch.command == 0x1100);
    pt.room = 64;
    CHECK(mc_dbdma_kick(&ch));
    mc_dbdma_run(&ch, 16);
    CHECK(pt.n == 5 && ch.command == 0x1110);
    /* INPUT_LAST ended early by the device: the residual count. */
    mc_dbdma_write(&ch, MC_DBDMA_CONTROL, 4, 0x80000000u);
    memcpy(pt.in, "xyz", 3);
    pt.in_n = 3;
    pt.in_pos = 0;
    pt.end = 1;
    command(0x1200, MC_DBDMA_INPUT_LAST, 0, 0, 0, 0, 16, 0x300, 0);
    command(0x1210, MC_DBDMA_STOP, 0, 0, 0, 0, 0, 0, 0);
    mc_dbdma_write(&ch, MC_DBDMA_COMMAND, 4, 0x1200);
    mc_dbdma_write(&ch, MC_DBDMA_CONTROL, 4, 0x80008000u);
    mc_dbdma_run(&ch, 16);
    CHECK(!memcmp(mem + 0x300, "xyz", 3));
    CHECK((get32le(0x120C) & 0xFFFF) == 13);
    /* A NOP branching on s0 (BranchSelect), the interrupt if s1 clear, a
     * wait on s2 (Tables 339-342). */
    mc_dbdma_write(&ch, MC_DBDMA_CONTROL, 4, 0x80000000u);
    command(0x1300, MC_DBDMA_NOP, 0, 2, 1, 0, 0, 0, 0x1340);
    command(0x1310, MC_DBDMA_STOP, 0, 0, 0, 0, 0, 0, 0);
    command(0x1340, MC_DBDMA_NOP, 0, 0, 0, 1, 0, 0, 0);
    command(0x1350, MC_DBDMA_STOP, 0, 0, 0, 0, 0, 0, 0);
    mc_dbdma_write(&ch, MC_DBDMA_BRANCH_SELECT, 4, 0x00010001u);            /* condition: s0 set */
    mc_dbdma_write(&ch, MC_DBDMA_INT_SELECT, 4, 0x00020002u);               /* condition: s1 set */
    mc_dbdma_write(&ch, MC_DBDMA_WAIT_SELECT, 4, 0x00040004u);              /* condition: s2 set */
    mc_dbdma_write(&ch, MC_DBDMA_COMMAND, 4, 0x1300);
    irqs = 0;
    mc_dbdma_write(&ch, MC_DBDMA_CONTROL, 4, 0x80058005u);                  /* run, s0, s2 */
    mc_dbdma_run(&ch, 16);
    CHECK(irqs == 1);                                                        /* i = 2, s1 clear */
    CHECK((get32le(0x130C) >> 16) & MC_DBDMA_BT);                           /* branch taken */
    CHECK(ch.command == 0x1340 && ch.waiting);                               /* waits while s2 */
    mc_dbdma_write(&ch, MC_DBDMA_CONTROL, 4, 0x00040000u);                  /* s2 clear: on */
    CHECK(!ch.waiting && ch.command == 0x1350);
    mc_dbdma_run(&ch, 16);
    CHECK(!(ch.status & MC_DBDMA_ACTIVE));
    /* A bus error: dead, an interrupt; clearing run clears dead. */
    irqs = 0;
    mc_dbdma_write(&ch, MC_DBDMA_CONTROL, 4, 0x80000000u);
    command(0x1400, MC_DBDMA_OUTPUT_LAST, 0, 0, 0, 0, 4, 0xFFFFFF00u, 0);
    mc_dbdma_write(&ch, MC_DBDMA_COMMAND, 4, 0x1400);
    mc_dbdma_write(&ch, MC_DBDMA_CONTROL, 4, 0x80008000u);
    mc_dbdma_run(&ch, 16);
    CHECK((ch.status & (MC_DBDMA_DEAD | MC_DBDMA_ACTIVE)) == MC_DBDMA_DEAD && irqs == 1);
    mc_dbdma_write(&ch, MC_DBDMA_CONTROL, 4, 0x80000000u);
    CHECK(!(ch.status & MC_DBDMA_DEAD));
    /* The output record is delivered before its completion wait, once.
     * Device completion can then release that wait (15.7.1). */
    mc_dbdma_port keyed = {&pt, port_read, port_write, NULL, port_end};
    mc_dbdma_set_port(&ch, &keyed);
    pt.n = pt.ends = 0;
    mc_dbdma_write(&ch, MC_DBDMA_WAIT_SELECT, 4, 0x00200020u);
    command(0x1600, MC_DBDMA_OUTPUT_LAST, 0, 3, 0, 2, 3, 0x100, 0);
    command(0x1610, MC_DBDMA_STOP, 0, 0, 0, 0, 0, 0, 0);
    mc_dbdma_write(&ch, MC_DBDMA_COMMAND, 4, 0x1600);
    mc_dbdma_write(&ch, MC_DBDMA_CONTROL, 4, 0xFFFF8000u);
    CHECK(!mc_dbdma_run(&ch, 8));
    CHECK(pt.n == 3 && pt.ends == 1 && ch.waiting && ch.command == 0x1600);
    CHECK(get32le(0x160C) == 0);
    mc_dbdma_set_bits(&ch, 0x20, 0x20);
    mc_dbdma_run(&ch, 8);
    CHECK(pt.ends == 1 && ch.command == 0x1610 && !ch.waiting);
    CHECK((get32le(0x160C) >> 16) & MC_DBDMA_ACTIVE);
    mc_dbdma_write(&ch, MC_DBDMA_CONTROL, 4, 0x80000000u);
    /* A program looping on itself stops at its budget. */
    command(0x1500, MC_DBDMA_NOP, 0, 0, 3, 0, 0, 0, 0x1500);
    mc_dbdma_write(&ch, MC_DBDMA_COMMAND, 4, 0x1500);
    mc_dbdma_write(&ch, MC_DBDMA_CONTROL, 4, 0x80008000u);
    CHECK(mc_dbdma_run(&ch, 8) == 1);
}

/* --- Grand Central ----------------------------------------------------------------- */

typedef struct regdev { mc_dev dev; uint32_t last_off, last_val; unsigned last_size; } regdev;
static uint32_t rd_read(void *p, uint32_t o, unsigned s) { regdev *r = p; r->last_off = o; r->last_size = s; return 0x5A; }
static void rd_write(void *p, uint32_t o, unsigned s, uint32_t v) { regdev *r = p; r->last_off = o; r->last_size = s; r->last_val = v; }
static const mc_dev_ops rd_ops = {rd_read, rd_write, NULL, NULL, NULL};

static void grand_central(void) {
    fresh();
    mc_gc g;
    mc_gc_init(&g, &host, "gc");
    slot_dev[0] = &g;
    const mc_dev_ops *o = &mc_gc_ops;
    /* The interrupt collection: nothing until bit 31 (POST) and a source. */
    o->write(&g, MC_GC_MASK, 4, 0x80000000u);
    mc_gc_set_source(&g, MC_GC_IRQ_VIA, 1);
    CHECK(!lines[0][MC_GC_LINE_INT]);
    CHECK(o->read(&g, MC_GC_EVENTS, 4) == 1u << MC_GC_IRQ_VIA);
    CHECK(o->read(&g, MC_GC_LEVELS, 4) == 1u << MC_GC_IRQ_VIA);
    o->write(&g, MC_GC_MASK, 4, 0x80000000u | 1u << MC_GC_IRQ_VIA);
    CHECK(lines[0][MC_GC_LINE_INT]);
    o->write(&g, MC_GC_CLEAR, 4, 0x80000000u);                 /* the nanokernel's acknowledge */
    CHECK(!lines[0][MC_GC_LINE_INT]);
    mc_gc_set_source(&g, MC_GC_IRQ_VIA, 0);                    /* a level source: follows its input; */
    CHECK(lines[0][MC_GC_LINE_INT] && o->read(&g, MC_GC_EVENTS, 4) == 0);   /* its drop interrupts too */
    o->write(&g, MC_GC_CLEAR, 4, 0x80000000u);
    CHECK(!lines[0][MC_GC_LINE_INT]);
    o->write(&g, MC_GC_MASK, 4, 0x80000000u | 1u << MC_GC_IRQ_DMA_FLOPPY);
    mc_gc_set_source(&g, MC_GC_IRQ_DMA_FLOPPY, 1);             /* a DBDMA source: latched */
    mc_gc_set_source(&g, MC_GC_IRQ_DMA_FLOPPY, 0);
    CHECK(lines[0][MC_GC_LINE_INT] && o->read(&g, MC_GC_EVENTS, 4) == 2u);
    CHECK(o->read(&g, MC_GC_LEVELS, 4) == 2u);                 /* the levels show the latched edge */
    o->write(&g, MC_GC_CLEAR, 4, 0x80000000u);
    o->write(&g, MC_GC_CLEAR, 4, 2u);                          /* the tree's acknowledge: a change again */
    CHECK(lines[0][MC_GC_LINE_INT] && o->read(&g, MC_GC_EVENTS, 4) == 0);
    o->write(&g, MC_GC_CLEAR, 4, 0x80000000u);
    CHECK(!lines[0][MC_GC_LINE_INT]);
    o->write(&g, MC_GC_MASK, 4, 1u << MC_GC_IRQ_DMA_FLOPPY);   /* without bit 31: no interrupt */
    mc_gc_set_source(&g, MC_GC_IRQ_DMA_FLOPPY, 1);
    CHECK(!lines[0][MC_GC_LINE_INT]);
    CHECK(o->read(&g, MC_GC_MASK + 3, 1) == 0x00 && o->read(&g, MC_GC_MASK, 1) == 0x02);
    /* The devices' windows: MESH's registers at $10 steps. */
    regdev r = {.dev = {&rd_ops, &host, "mesh"}};
    CHECK(mc_gc_attach(&g, MC_GC_MESH, 0x100, 4, &r, &rd_ops));
    CHECK(o->read(&g, MC_GC_MESH + 0xE0, 1) == 0x5A && r.last_off == 14);
    o->write(&g, MC_GC_MESH + 0x30, 1, 0x81);
    CHECK(r.last_off == 3 && r.last_val == 0x81);
    CHECK(o->read(&g, MC_GC_SWIM3, 1) == 0);                    /* nobody there */
    /* A DBDMA channel through its registers: channel 10 (MESH) at $8A00,
     * its interrupt to source 10. */
    port pt = {.room = 64};
    const mc_dbdma_port dp = {&pt, port_read, port_write, NULL};
    mc_gc_dma_port(&g, 10, &dp);
    memcpy(mem + 0x100, "MESH", 4);
    command(0x2000, MC_DBDMA_OUTPUT_LAST, 0, 3, 0, 0, 4, 0x100, 0);
    command(0x2010, MC_DBDMA_STOP, 0, 0, 0, 0, 0, 0, 0);
    o->write(&g, MC_GC_MASK, 4, 0x80000000u | 1u << 10);
    o->write(&g, MC_GC_DBDMA + 0xA00 + MC_DBDMA_COMMAND, 4, 0x2000);
    o->write(&g, MC_GC_DBDMA + 0xA00 + MC_DBDMA_CONTROL, 4, 0x80008000u);
    CHECK(pt.n == 4 && !memcmp(pt.out, "MESH", 4));
    CHECK(o->read(&g, MC_GC_EVENTS, 4) & (1u << 10));
    CHECK(lines[0][MC_GC_LINE_INT]);
    CHECK(o->read(&g, MC_GC_DBDMA + 0xA00 + MC_DBDMA_COMMAND, 4) == 0x2010);
    /* A channel that loops continues from the controller's event. */
    command(0x2100, MC_DBDMA_NOP, 0, 0, 3, 0, 0, 0, 0x2100);
    o->write(&g, MC_GC_DBDMA + 0x100 + MC_DBDMA_COMMAND, 4, 0x2100);
    o->write(&g, MC_GC_DBDMA + 0x100 + MC_DBDMA_CONTROL, 4, 0x80008000u);
    CHECK(g.pending && due[0].dev == &g && due[0].when != MC_NEVER);
    o->write(&g, MC_GC_DBDMA + 0x100 + MC_DBDMA_CONTROL, 4, 0x80000000u);
    advance(1000);
    CHECK(!g.pending);
    /* The PCI function: the BAR is 1 MB, nothing until it is enabled. */
    const mc_pci_function f = mc_gc_function(&g);
    uint32_t v;
    CHECK(!f.mem_read(f.dev, 0xF3000024u, 4, &v));
    f.config_write(f.dev, MC_PCI_BAR0, 4, 0xFFFFFFFFu);
    CHECK(f.config_read(f.dev, MC_PCI_BAR0, 4) == 0xFFF00000u);
    f.config_write(f.dev, MC_PCI_BAR0, 4, 0xF3000000u);
    f.config_write(f.dev, MC_PCI_COMMAND, 2, 0x16);
    CHECK(f.mem_read(f.dev, 0xF3000024u, 4, &v) && v == g.mask);
}

/* --- MESH (CHRP I/O 12) ------------------------------------------------------------- */

typedef struct tgt { mc_scsi_target t; uint8_t last_cdb[12]; } tgt;
static uint8_t t_command(mc_scsi_target *t, unsigned lun, const uint8_t *cdb, unsigned length, int *direction, uint32_t *bytes) {
    tgt *g = (tgt *)t;
    (void)lun;
    memcpy(g->last_cdb, cdb, length < 12 ? length : 12);
    if (cdb[0] == 0x12) { *direction = MC_SCSI_XFER_IN; *bytes = cdb[4]; }
    else { *direction = MC_SCSI_XFER_NONE; *bytes = 0; }
    return MC_SCSI_GOOD;
}
static int t_in(mc_scsi_target *t, uint8_t *buf, uint32_t offset, uint32_t count) {
    (void)t;
    for (uint32_t i = 0; i < count; i++) buf[i] = (uint8_t)(0x40 + offset + i);
    return 0;
}
static int t_out(mc_scsi_target *t, const uint8_t *buf, uint32_t offset, uint32_t count) { (void)t; (void)buf; (void)offset; (void)count; return 0; }
static uint8_t t_finish(mc_scsi_target *t) { (void)t; return MC_SCSI_GOOD; }
static void t_reset(mc_scsi_target *t) { (void)t; }

static void mesh(void) {
    fresh();
    mc_scsi_bus bus;
    tgt t = {{t_command, t_in, t_out, t_finish, t_reset}, {0}};
    mc_scsi_bus_init(&bus);
    mc_scsi_bus_attach(&bus, 0, &t.t);
    mc_mesh m;
    mc_mesh_init(&m, &host, "mesh", &bus, 0x02);
    slot_dev[1] = &m;
    mc_mesh_reset(&m);
    const mc_dev_ops *o = &mc_mesh_ops;
    CHECK(lines[1][MC_MESH_LINE_DMA_STATUS] == 0xC0);
    /* 12.5.11: after reset CmdDone is set and the mask clear (no IRQ). */
    CHECK(o->read(&m, MC_MESH_INTERRUPT, 1) == MC_MESH_INT_DONE && !lines[1][MC_MESH_LINE_IRQ]);
    CHECK(o->read(&m, MC_MESH_ID, 1) == 0x02);
    o->write(&m, MC_MESH_INTERRUPT, 1, 7);
    CHECK(lines[1][MC_MESH_LINE_DMA_STATUS] == 0xE0);
    o->write(&m, MC_MESH_INT_MASK, 1, 7);
    o->write(&m, MC_MESH_SOURCE_ID, 1, 7);
    o->write(&m, MC_MESH_SEL_TIMEOUT, 1, 25);
    /* Arbitrate, then select an absent target: SelTO after 25 x 10 ms. */
    o->write(&m, MC_MESH_SEQUENCE, 1, MC_MESH_CMD_ARBITRATE);
    CHECK(o->read(&m, MC_MESH_INTERRUPT, 1) == 0);              /* issuing clears CmdDone */
    advance(now + 100);
    CHECK(o->read(&m, MC_MESH_INTERRUPT, 1) == MC_MESH_INT_DONE && lines[1][MC_MESH_LINE_IRQ]);
    o->write(&m, MC_MESH_INTERRUPT, 1, MC_MESH_INT_DONE);
    CHECK(!lines[1][MC_MESH_LINE_IRQ]);
    o->write(&m, MC_MESH_DEST_ID, 1, 3);
    o->write(&m, MC_MESH_SEQUENCE, 1, MC_MESH_CMD_SELECT);
    advance(now + MC_C15M_HZ / 4 - 1000);
    CHECK(o->read(&m, MC_MESH_EXCEPTION, 1) == 0);
    advance(now + 2000);
    CHECK(o->read(&m, MC_MESH_EXCEPTION, 1) == MC_MESH_EXC_SELTO && o->read(&m, MC_MESH_INTERRUPT, 1) == (MC_MESH_INT_EXCEPTION | MC_MESH_INT_DONE));
    CHECK(lines[1][MC_MESH_LINE_DMA_STATUS] == 0x80);
    o->write(&m, MC_MESH_INTERRUPT, 1, MC_MESH_INT_DONE);
    o->write(&m, MC_MESH_EXCEPTION, 1, MC_MESH_EXC_SELTO);     /* cleared by writing ones */
    CHECK(o->read(&m, MC_MESH_INTERRUPT, 1) == 0 && !lines[1][MC_MESH_LINE_IRQ]);
    /* Target 0 with ATN: MSG OUT (IDENTIFY), COMMAND (INQUIRY, 6 bytes
     * through the FIFO), DATA IN by DMA, STATUS, MSG IN, bus free. */
    o->write(&m, MC_MESH_SEQUENCE, 1, MC_MESH_CMD_ARBITRATE);
    advance(now + 100);
    o->write(&m, MC_MESH_INTERRUPT, 1, 7);
    o->write(&m, MC_MESH_DEST_ID, 1, 0);
    o->write(&m, MC_MESH_SEQUENCE, 1, (unsigned)MC_MESH_CMD_SELECT | MC_MESH_SEQ_ATN);
    advance(now + 100);
    CHECK(o->read(&m, MC_MESH_INTERRUPT, 1) == MC_MESH_INT_DONE);
    CHECK((o->read(&m, MC_MESH_STATUS0, 1) & 0x27) == (0x20 | MC_SCSI_MSG_OUT));
    CHECK(o->read(&m, MC_MESH_STATUS1, 1) & 0x40);                 /* BSY */
    o->write(&m, MC_MESH_COUNT0, 1, 1);
    o->write(&m, MC_MESH_COUNT1, 1, 0);
    o->write(&m, MC_MESH_SEQUENCE, 1, MC_MESH_CMD_MSG_OUT);
    o->write(&m, MC_MESH_FIFO, 1, 0x80);
    advance(now + 100);
    CHECK(o->read(&m, MC_MESH_INTERRUPT, 1) == MC_MESH_INT_DONE);
    CHECK((o->read(&m, MC_MESH_STATUS0, 1) & 7) == MC_SCSI_COMMAND);
    static const uint8_t inquiry[6] = {0x12, 0, 0, 0, 5, 0};
    o->write(&m, MC_MESH_COUNT0, 1, 6);
    o->write(&m, MC_MESH_SEQUENCE, 1, MC_MESH_CMD_COMMAND);
    for (int i = 0; i < 6; i++) o->write(&m, MC_MESH_FIFO, 1, inquiry[i]);
    advance(now + 100);
    CHECK(t.last_cdb[0] == 0x12 && o->read(&m, MC_MESH_INTERRUPT, 1) == MC_MESH_INT_DONE);
    CHECK((o->read(&m, MC_MESH_STATUS0, 1) & 7) == MC_SCSI_DATA_IN);
    o->write(&m, MC_MESH_COUNT0, 1, 5);
    o->write(&m, MC_MESH_SEQUENCE, 1, (unsigned)MC_MESH_CMD_DATA_IN | MC_MESH_SEQ_DMA);
    CHECK(lines[1][MC_MESH_LINE_DRQ]);
    uint8_t b[8];
    int k = 0;
    while (k < 8 && mc_mesh_dma_read(&m, &b[k]) == MC_DBDMA_BYTE) k++;
    CHECK(k == 5 && b[0] == 0x40 && b[4] == 0x44);
    CHECK(o->read(&m, MC_MESH_COUNT0, 1) == 0);
    advance(now + 100);
    CHECK(o->read(&m, MC_MESH_INTERRUPT, 1) == MC_MESH_INT_DONE && !lines[1][MC_MESH_LINE_DRQ]);
    /* G3 Open Firmware asks for Status with a zero (64 KB) count and
     * waits for PhaseMM before reading the one-byte reply from the FIFO. */
    o->write(&m, MC_MESH_COUNT0, 1, 0);
    o->write(&m, MC_MESH_SEQUENCE, 1, MC_MESH_CMD_STATUS);
    advance(now + 100);
    CHECK(o->read(&m, MC_MESH_EXCEPTION, 1) == MC_MESH_EXC_PHASE &&
          o->read(&m, MC_MESH_INTERRUPT, 1) == (MC_MESH_INT_EXCEPTION | MC_MESH_INT_DONE) &&
          lines[1][MC_MESH_LINE_IRQ]);
    CHECK(o->read(&m, MC_MESH_COUNT0, 1) == 0xFF && o->read(&m, MC_MESH_COUNT1, 1) == 0xFF);
    CHECK(o->read(&m, MC_MESH_FIFO_COUNT, 1) == 1 && o->read(&m, MC_MESH_FIFO, 1) == MC_SCSI_GOOD);
    o->write(&m, MC_MESH_EXCEPTION, 1, MC_MESH_EXC_PHASE);
    o->write(&m, MC_MESH_COUNT1, 1, 0);
    o->write(&m, MC_MESH_COUNT0, 1, 1);
    o->write(&m, MC_MESH_SEQUENCE, 1, MC_MESH_CMD_MSG_IN);
    CHECK(o->read(&m, MC_MESH_FIFO, 1) == MC_SCSI_MSG_COMMAND_COMPLETE);
    advance(now + 100);
    CHECK(o->read(&m, MC_MESH_STATUS0, 1) & 0x10);                 /* ACK held after the input */
    o->write(&m, MC_MESH_SEQUENCE, 1, MC_MESH_CMD_BUS_FREE);
    advance(now + 100);
    CHECK(o->read(&m, MC_MESH_INTERRUPT, 1) == MC_MESH_INT_DONE);
    CHECK(!(o->read(&m, MC_MESH_STATUS1, 1) & 0x40));
    /* A phase mismatch: DATA OUT asked with the bus free. */
    o->write(&m, MC_MESH_INT_MASK, 1, 0);
    o->write(&m, MC_MESH_SEQUENCE, 1, MC_MESH_CMD_DATA_OUT);
    /* AppleMesh initCP + BR_IF_PROBLEM: wait for CmdDone (s5 low),
     * then branch on Exception/Error (s6/s7 low), with CPU IRQ masked. */
    const mc_dbdma_host dh = {NULL, dma_mem, dma_irq};
    mc_dbdma ch;
    mc_dbdma_init(&ch, &dh, 0, NULL);
    mesh_dma_channel = &ch;
    mc_dbdma_set_bits(&ch, 0xE0, (uint8_t)lines[1][MC_MESH_LINE_DMA_STATUS]);
    command(0x1800, MC_DBDMA_NOP, 0, 0, 2, 1, 0, 0, 0x1840);
    command(0x1810, MC_DBDMA_STOP, 0, 0, 0, 0, 0, 0, 0);
    command(0x1840, MC_DBDMA_STOP, 0, 0, 0, 0, 0, 0, 0);
    mc_dbdma_write(&ch, MC_DBDMA_WAIT_SELECT, 4, 0x00200020);
    mc_dbdma_write(&ch, MC_DBDMA_BRANCH_SELECT, 4, 0x00C000C0);
    mc_dbdma_write(&ch, MC_DBDMA_COMMAND, 4, 0x1800);
    mc_dbdma_write(&ch, MC_DBDMA_CONTROL, 4, 0x80008000);
    mc_dbdma_run(&ch, 16);
    CHECK(ch.waiting && ch.command == 0x1800);
    advance(now + 100);
    CHECK(!ch.waiting && ch.command == 0x1840 && (ch.status & MC_DBDMA_BT));
    CHECK(!lines[1][MC_MESH_LINE_IRQ] && lines[1][MC_MESH_LINE_DMA_STATUS] == 0x80);
    mesh_dma_channel = NULL;
    CHECK(o->read(&m, MC_MESH_EXCEPTION, 1) == MC_MESH_EXC_PHASE);
    o->write(&m, MC_MESH_SEQUENCE, 1, MC_MESH_CMD_ARBITRATE);     /* 12.5.9: with an exception pending */
    CHECK(o->read(&m, MC_MESH_ERROR, 1) & MC_MESH_ERR_SEQ);
    /* RstMESH: the controller, CmdDone, the mask cleared. */
    o->write(&m, MC_MESH_SEQUENCE, 1, MC_MESH_CMD_RESET);
    CHECK(o->read(&m, MC_MESH_INTERRUPT, 1) == MC_MESH_INT_DONE && o->read(&m, MC_MESH_INT_MASK, 1) == 0 && !lines[1][MC_MESH_LINE_IRQ]);
    /* The SCSI reset through bus status 1. */
    o->write(&m, MC_MESH_INT_MASK, 1, 7);
    o->write(&m, MC_MESH_STATUS1, 1, 0x80);
    CHECK((o->read(&m, MC_MESH_ERROR, 1) & MC_MESH_ERR_RESET) && lines[1][MC_MESH_LINE_IRQ]);
    o->write(&m, MC_MESH_STATUS1, 1, 0);
    mc_scsi_bus_free(&bus);
}

/* --- Hammerhead and Platinum: banks as POST sizes them ------------------------------ */

static void memory_controllers(void) {
    fresh();
    mc_hammerhead h;
    const uint32_t banks[MC_HAMMERHEAD_BANKS] = {8u << 20, 0, 16u << 20};
    mc_hammerhead_init(&h, &host, "hammerhead", 0x00000120u, MC_HAMMERHEAD_L2_256K, banks);
    slot_dev[2] = &h;
    const mc_dev_ops *o = &mc_hammerhead_ops;
    CHECK(o->read(&h, 0, 4) == 0x00000120u && (o->read(&h, 0, 4) >> 16) != 0x3001u);
    CHECK(o->read(&h, MC_HAMMERHEAD_L2, 1) == 0x81 && o->read(&h, MC_HAMMERHEAD_L2, 4) == 0x81000000u);
    o->write(&h, MC_HAMMERHEAD_L2, 4, 0x70000000u);             /* POST's enable: pins kept */
    CHECK(o->read(&h, MC_HAMMERHEAD_L2, 1) == 0xF1);
    /* $FFF0402C: bank n at n x 64 MB. */
    for (unsigned n = 0; n < MC_HAMMERHEAD_BANKS; n++) {
        o->write(&h, MC_HAMMERHEAD_BANK + 0x20 * n, 4, ((n >> 4) & 1u) << 24);
        o->write(&h, MC_HAMMERHEAD_BANK + 0x20 * n + 0x10, 4, n << 28);
    }
    CHECK(rises[2][MC_HAMMERHEAD_LINE_MAP] == 2 * MC_HAMMERHEAD_BANKS);
    CHECK(mc_hammerhead_base(&h, 2) == 0x08000000u && mc_hammerhead_base(&h, 17) == 17u * 0x04000000u);
    unsigned bank;
    uint32_t off;
    CHECK(mc_hammerhead_decode(&h, 0x00000010u, &bank, &off) && bank == 0 && off == 0x10);
    CHECK(mc_hammerhead_decode(&h, 0x03FFFFF8u, &bank, &off) && bank == 0 && off == (8u << 20) - 8);  /* aliases */
    CHECK(!mc_hammerhead_decode(&h, 0x04000000u, NULL, NULL));  /* bank 1 empty: floats */
    CHECK(mc_hammerhead_decode(&h, 0x08000004u, &bank, &off) && bank == 2 && off == (8u << 20) + 4);
    CHECK(mc_hammerhead_decode(&h, 0x0BFFFFFCu, &bank, &off) && off == (8u << 20) + (16u << 20) - 4);
    /* Packed: bank 0 at 0, the others at 8 MB (2 in 4 MB units). */
    for (unsigned n = 1; n < MC_HAMMERHEAD_BANKS; n++) {
        o->write(&h, MC_HAMMERHEAD_BANK + 0x20 * n, 4, 0);
        o->write(&h, MC_HAMMERHEAD_BANK + 0x20 * n + 0x10, 4, 2u << 24);
    }
    mc_mem_window w[4];
    CHECK(mc_hammerhead_windows(&h, w, 4) == 2);
    CHECK(w[0].base == 0 && w[0].size == (8u << 20) && w[1].base == (8u << 20) && w[1].size == 0x04000000u);
    CHECK(mc_hammerhead_decode(&h, (8u << 20) + 0x100, &bank, &off) && bank == 2 && off == (8u << 20) + 0x100);

    /* 9500 Developer Note, "Data Interleaving": two adjacent equal
     * DIMMs form a 128-bit array. The 8500 ROM's final writes at
     * $FFF042D4-$FFF042F4 join two 64 MB DIMMs and park bank 1 at 128 MB. */
    const uint32_t paired[MC_HAMMERHEAD_BANKS] = {64u << 20, 64u << 20};
    mc_hammerhead_init(&h, &host, "hammerhead", 0x00000120u, MC_HAMMERHEAD_L2_256K, paired);
    o->write(&h, MC_HAMMERHEAD_BANK + 0x30, 4, 0x10000000u); /* separate bank 1 at 64 MB */
    CHECK(!mc_hammerhead_interleaved(&h, 0));
    uint8_t lanes[32] = {0};
    static const uint32_t separate[] = {0, 8, 0x04000000u, 0x04000008u};
    static const uint32_t lane_at[] = {0, 16, 8, 24};
    for (unsigned i = 0; i < 4; i++) {
        CHECK(mc_hammerhead_decode(&h, separate[i], &bank, &off));
        CHECK(bank == i / 2 && off == lane_at[i]);
        if (off < sizeof lanes) lanes[off] = (uint8_t)(0xA0u + i);
    }
    o->write(&h, MC_HAMMERHEAD_BANK, 4, 0x04000000u);
    o->write(&h, MC_HAMMERHEAD_BANK + 0x30, 4, 0x20000000u);
    CHECK(mc_hammerhead_interleaved(&h, 0));
    CHECK(!mc_hammerhead_interleaved(&h, 1));
    CHECK(mc_hammerhead_windows(&h, w, 4) == 1);
    CHECK(w[0].base == 0 && w[0].size == (128u << 20) && w[0].bank_size == (128u << 20));
    static const uint8_t combined[] = {0xA0, 0xA2, 0xA1, 0xA3};
    for (unsigned i = 0; i < 4; i++) {
        CHECK(mc_hammerhead_decode(&h, 8u * i, &bank, &off));
        CHECK(bank == (i & 1u) && off == 8u * i);
        CHECK(off < sizeof lanes && lanes[off] == combined[i]);
    }
    CHECK(mc_hammerhead_decode(&h, 0x068E7780u, &bank, &off) && off == 0x068E7780u);
    CHECK(mc_hammerhead_decode(&h, 0x07FFFFF8u, &bank, &off) && bank == 1 && off == 0x07FFFFF8u);
    CHECK(!mc_hammerhead_decode(&h, 0x08000000u, NULL, NULL));
    o->write(&h, MC_HAMMERHEAD_BANK, 4, 0);
    o->write(&h, MC_HAMMERHEAD_BANK + 0x30, 4, 0x10000000u);
    for (unsigned i = 0; i < 4; i++) {
        CHECK(mc_hammerhead_decode(&h, separate[i], &bank, &off));
        CHECK(off < sizeof lanes && lanes[off] == 0xA0u + i);
    }

    mc_platinum p;
    const uint32_t pbanks[MC_PLATINUM_BANKS] = {16u << 20};
    CHECK(mc_platinum_init(&p, &host, "platinum", 0x0120, MC_PLATINUM_L2_256K, pbanks, 2u << 20));
    slot_dev[3] = &p;
    CHECK(mc_platinum_ops.read(&p, 0, 4) == 0x30010120u && mc_platinum_ops.read(&p, 0, 2) == 0x3001u);
    CHECK((mc_platinum_ops.read(&p, MC_PLATINUM_CACHE, 4) & 0x38u) == 0x28u);   /* present, 256 KB */
    mc_platinum_ops.write(&p, MC_PLATINUM_CACHE, 4, 0x05);                       /* tag mode, enable */
    CHECK(mc_platinum_ops.read(&p, MC_PLATINUM_CACHE, 4) == 0x2Du);
    for (unsigned n = 0; n < MC_PLATINUM_BANKS; n++) mc_platinum_ops.write(&p, MC_PLATINUM_BANK + 0x10 * n, 4, (n << 27) & 0x7FE00000u);
    CHECK(mc_platinum_base(&p, 1) == 0x08000000u);
    CHECK(mc_platinum_windows(&p, w, 4) == 1 && w[0].size == MC_PLATINUM_BANK_WINDOW && w[0].bank_size == (16u << 20));
    /* VRAM aliasing at 2 MB ("Rush2112" at $F120 0000 shows at $F100 0000). */
    mc_platinum_vram_ops.write(&p, 0x200000, 4, 0x52757368u);
    CHECK(mc_platinum_vram_ops.read(&p, 0, 4) == 0x52757368u);
    advance(MC_C15M_HZ / 60u + 1);
    CHECK(mc_platinum_ops.read(&p, MC_PLATINUM_VBL_STATUS, 4) & 4u);
    CHECK(lines[3][MC_PLATINUM_LINE_VBL]);
    mc_platinum_ops.read(&p, MC_PLATINUM_VBL_ACK, 4);
    CHECK(!(mc_platinum_ops.read(&p, MC_PLATINUM_VBL_STATUS, 4) & 4u) && !lines[3][MC_PLATINUM_LINE_VBL]);
    advance(now + MC_C15M_HZ / 60u / 50u + 1);
    CHECK(!(mc_platinum_ops.read(&p, MC_PLATINUM_VBL_STATUS, 4) & 4u) && !lines[3][MC_PLATINUM_LINE_VBL]);
    advance(MC_C15M_HZ / 60u * 2u + 1);
    CHECK(lines[3][MC_PLATINUM_LINE_VBL]);
    /* With no ACK, the end of blanking must still release Grand Central. */
    advance(now + MC_C15M_HZ / 60u / 50u + 1);
    CHECK(!(mc_platinum_ops.read(&p, MC_PLATINUM_VBL_STATUS, 4) & 4u) && !lines[3][MC_PLATINUM_LINE_VBL]);
    mc_platinum_free(&p);
}

/* --- the sound interface, NVRAM, the DAC, Control, Plan B -------------------------- */

static uint32_t words[8];
static unsigned nwords;
static void codec_control(void *ctx, uint32_t word) { (void)ctx; if (nwords < 8) words[nwords++] = word; }
static uint32_t codec_status(void *ctx) { (void)ctx; return 0x00000008u; }

static void sound_and_video(void) {
    fresh();
    mc_awacs a;
    const mc_awacs_codec codec = {NULL, codec_control, codec_status};
    nwords = 0;
    mc_awacs_init(&a, &host, "awacs", &codec);
    slot_dev[4] = &a;
    const mc_dev_ops *o = &mc_awacs_ops;
    /* POST's chime: the rate code 2, the codec words, the busy bit. */
    o->write(&a, MC_AWACS_CONTROL, 4, 2u << 8);
    CHECK(mc_awacs_rate(&a) == 22050u);
    o->write(&a, MC_AWACS_CODEC, 4, 0x4000u + 4u * 0x41u);
    CHECK(nwords == 1 && words[0] == 0x4104u);
    CHECK(o->read(&a, MC_AWACS_CODEC, 4) & MC_AWACS_BUSY);
    advance(now + MC_C15M_HZ / 22050u + 2);
    CHECK(!(o->read(&a, MC_AWACS_CODEC, 4) & MC_AWACS_BUSY));
    CHECK(o->read(&a, MC_AWACS_STATUS, 4) == 8u);
    /* Frames from the output channel's bytes, big-endian, left first. */
    static const uint8_t frame[8] = {0x12, 0x34, 0xFE, 0xDC, 0x00, 0x01, 0x80, 0x00};
    for (int i = 0; i < 8; i++) CHECK(mc_awacs_dma_write(&a, frame[i]));
    mc_awacs_pull(&a, NULL, 0);
    a.ring_head = a.ring_n = 0;
    advance(now + MC_C15M_HZ / 1000u);
    int16_t s[64];
    const size_t n = mc_awacs_pull(&a, s, 32);
    CHECK(n >= 20 && n <= 24);
    CHECK(s[0] == 0x1234 && s[1] == -0x124 && s[2] == 1 && s[3] == -32768 && s[4] == 0);
    CHECK(rises[4][MC_AWACS_LINE_OUT_DRQ] >= 1);

    mc_nvram nv;
    mc_nvram_init(&nv, &host, "nvram", NULL);
    nv.data[0x1308] = 5;                                         /* parameter RAM byte 8: the volume */
    mc_nvram_page_ops.write(&nv, 0, 4, 0x1308u >> 5);            /* $FFF035E0 */
    CHECK(mc_nvram_data_ops.read(&nv, (0x1308u & 31u) << 4, 1) == 5);
    mc_nvram_page_ops.write(&nv, 0, 4, 0x1044u >> 5);            /* $FFF03608: the RAM size, 4 bytes */
    for (unsigned i = 0; i < 4; i++) mc_nvram_data_ops.write(&nv, ((0x1044u & 31u) + i) << 4, 1, i == 1 ? 2u : 0u);
    CHECK(nv.data[0x1045] == 2 && nv.dirty);

    mc_radacal d;
    mc_radacal_init(&d, &host, "dacula", MC_DAC_DACULA);
    mc_radacal_ops.write(&d, MC_DAC_ADDRESS, 1, MC_DAC_ID_REGISTER);
    CHECK(mc_radacal_ops.read(&d, MC_DAC_CONTROL, 1) == MC_DACULA_ID);
    mc_radacal_ops.write(&d, MC_DAC_ADDRESS, 1, 0xFE);
    const uint8_t rgb[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
    for (int i = 0; i < 6; i++) mc_radacal_ops.write(&d, MC_DAC_CLUT, 1, rgb[i]);
    CHECK(d.palette[0xFE] == 0x112233u && d.palette[0xFF] == 0x445566u && d.address == 0);
    mc_radacal_ops.write(&d, MC_DAC_ADDRESS, 1, 0x21);
    mc_radacal_ops.write(&d, MC_DAC_CONTROL, 1, 1);
    CHECK(mc_radacal_ops.read(&d, MC_DAC_CONTROL, 1) == 1);

    mc_control_chip c;
    CHECK(mc_control_init(&c, &host, "control", 2u << 20));
    slot_dev[5] = &c;
    const mc_pci_function f = mc_control_function(&c);
    CHECK(f.config_read(f.dev, 0, 4) == 0x0003106Bu);
    CHECK((f.config_read(f.dev, 8, 4) >> 8) == 0x038000u);
    f.config_write(f.dev, 0x14, 4, 0xFFFFFFFFu);
    CHECK(f.config_read(f.dev, 0x14, 4) == 0xFFFFF000u);
    f.config_write(f.dev, 0x18, 4, 0xFFFFFFFFu);
    CHECK(f.config_read(f.dev, 0x18, 4) == 0xFC000000u);
    f.config_write(f.dev, 0x14, 4, 0xF0000000u);
    f.config_write(f.dev, 0x18, 4, 0x84000000u);
    f.config_write(f.dev, MC_PCI_COMMAND, 2, MC_PCI_CMD_MEMORY);
    /* Open Firmware's 640 x 480 at 67 Hz (file $34684A) and 8 bits. */
    static const uint16_t timing[16] = {0x415, 0x412, 0x52, 0x2B, 0x4, 0x418, 0x41A, 0x48, 0x1AE, 0x189, 0x49, 0x1F, 0x1AF, 0x10, 0xD8, 0x190};
    for (unsigned i = 0; i < 16; i++) CHECK(f.mem_write(f.dev, 0xF0000000u + 0x10 + 0x10 * i, 4, timing[i]));
    f.mem_write(f.dev, 0xF0000000u + MC_CONTROL_STRIDE, 4, 640 + 0x20);
    f.mem_write(f.dev, 0xF0000000u + MC_CONTROL_PIXEL, 4, 2);
    uint32_t v = 0;
    CHECK(f.mem_read(f.dev, 0xF0000000u + MC_CONTROL_STRIDE, 4, &v) && v == 0x2A0u);
    mc_control_view view;
    CHECK(mc_control_view_get(&c, &view) && view.width == 640 && view.height == 480 && view.depth == 8 && view.row_bytes == 0x2A0);
    f.mem_write(f.dev, 0xF0000000u + MC_CONTROL_STRIDE, 4, (640 * 4 + 0x20) * 2);   /* 32 bits, the doubled stride */
    CHECK(mc_control_view_get(&c, &view) && view.depth == 32 && view.row_bytes == 640 * 4 + 0x20);
    CHECK(f.mem_write(f.dev, 0x84800000u, 4, 0x6F6E614Eu));      /* "Nano" through the bus (little-endian value) */
    CHECK(!memcmp(c.vram, "Nano", 4));
    CHECK(f.mem_read(f.dev, 0x84A00000u, 4, &v) && v == 0x6F6E614Eu);    /* 2 MB: aliases */
    CHECK(!f.mem_read(f.dev, 0x88000000u, 4, &v));
    f.mem_write(f.dev, 0xF0000000u + MC_CONTROL_INT, 4, 4);
    advance(now + MC_C15M_HZ / 60u + 1);
    CHECK(c.reg[MC_CONTROL_STATUS >> 4] & 4u);
    CHECK(lines[5][MC_CONTROL_LINE_VBL]);
    f.mem_write(f.dev, 0xF0000000u + MC_CONTROL_INT, 4, 0xC);
    CHECK(!lines[5][MC_CONTROL_LINE_VBL]);
    mc_control_free(&c);

    mc_planb pb;
    mc_planb_init(&pb, &host, "planb");
    const mc_pci_function pf = mc_planb_function(&pb);
    CHECK(pf.config_read(pf.dev, 0, 4) == 0x0004106Bu);
    pf.config_write(pf.dev, MC_PCI_BAR0, 4, 0xFFFFFFFFu);
    CHECK(pf.config_read(pf.dev, MC_PCI_BAR0, 4) == 0xFFFFFC00u);
    pf.config_write(pf.dev, MC_PCI_BAR0, 4, 0xF1000000u);
    pf.config_write(pf.dev, MC_PCI_COMMAND, 2, MC_PCI_CMD_MEMORY);
    CHECK(pf.mem_read(pf.dev, 0xF1000010u, 4, &v) && v == 0);
}

/* The actual boards must identify their codec through Grand Central's
 * PCI window, including after reset. An absent codec ID makes the native
 * sound plug-in reject the device before it installs input or output. */
static void board_identity(void) {
    static const char *const keys[] = {
        "power-macintosh-7200", "power-macintosh-7500", "power-macintosh-8500",
        "power-macintosh-7600", "power-macintosh-8600", "power-macintosh-8600-300",
        "power-macintosh-7300", "power-macintosh-9500", "power-macintosh-9600", "power-macintosh-9600-350",
    };
    uint8_t *rom = calloc(1, 4u << 20);
    CHECK(rom != NULL);
    if (!rom) return;
    rom[0] = 0x96; rom[1] = 0x30; rom[2] = 0xC6; rom[3] = 0x8B;
    for (unsigned i = 0; i < sizeof keys / sizeof *keys; i++) {
        mc_config config;
        mc_config_defaults(&config);
        config.controller = MC_CONTROLLER_PROTOCOL;
        config.nvram[0] = config.source[0] = 0;
        mc_config_set_ram_kb(&config, 32u << 10);
        const mc_ppc_desc *d = mc_ppc_find_board(keys[i]);
        CHECK(d != NULL);
        if (!d) continue;
        char error[256] = "";
        mc_ppc *m = mc_ppc_create(d, &config, rom, 4u << 20, error, sizeof error);
        CHECK(m != NULL);
        if (!m) { fprintf(stderr, "%s: %s\n", keys[i], error); continue; }
        for (unsigned reset = 0; reset < 2; reset++) {
            if (reset) d->ops->reset(m->board);
            uint32_t v = 0;
            CHECK(!mc_bus_write(&m->bus, 0xF2800000u, 4, 0x10000100u));
            CHECK(!mc_bus_write(&m->bus, 0xF2C00000u, 4, 0x000000F3u));
            CHECK(!mc_bus_write(&m->bus, 0xF2800000u, 4, 0x04000100u));
            CHECK(!mc_bus_write(&m->bus, 0xF2C00000u, 4, 0x16000000u));
            /* The ROM's normal-start strap must remain high after reset.
             * System 8 gpch 201 distinguishes the 7300 from the 7500 via
             * native word bit 23; pin 21 retains the common board class. */
            CHECK(!mc_bus_read(&m->bus, 0xF301A000u, 4, &v) && (v & 0x00010000u));
            if (!strcmp(keys[i], "power-macintosh-7300"))
                CHECK((v & 0x00A00000u) == 0x00200000u);
            if (!strcmp(keys[i], "power-macintosh-7500")) CHECK(v & 0x00800000u);
            /* PCI address invariance: bytes 00 20 00 00; a byte-reversed
             * word load by the native driver reads status $00002000. */
            CHECK(!mc_bus_read(&m->bus, 0xF3014020u, 4, &v) && v == 0x00200000u);
            CHECK(!mc_bus_read(&m->bus, 0xF3014021u, 1, &v) && v == 0x20u);
            CHECK(!mc_bus_write(&m->bus, 0xF3014020u, 4, 0));
            CHECK(!mc_bus_read(&m->bus, 0xF3014020u, 4, &v) && v == 0x00200000u);
        }
        mc_ppc_destroy(m);
    }
    free(rom);
}

/* --- a PCI ROM's POST on a test board of these chips ------------------------------ */

typedef struct pboard {
    mc_ppc *m;
    mc_bandit bandit, chaos;
    mc_gc gc;
    mc_hammerhead hh;
    mc_nvram nvram;
    mc_awacs awacs;
    mc_radacal dac;
    mc_control_chip control;
    mc_planb planb;
    mc_via via;
    unsigned remaps;
} pboard;
static pboard pb_board;

static void remap(pboard *b) {
    mc_mem_window w[MC_HAMMERHEAD_BANKS];
    const unsigned n = mc_hammerhead_windows(&b->hh, w, MC_HAMMERHEAD_BANKS);
    mc_bus_map_float(&b->m->bus, 0, 0x80000000u);
    for (unsigned i = 0; i < n; i++)
        if (w[i].base < 0x80000000u)
            mc_bus_map_memory(&b->m->bus, w[i].base, w[i].size, b->m->ram + w[i].offset, w[i].bank_size, 1);
    b->remaps++;
}
static void pb_line(void *p, void *dev, int line, int state) {
    pboard *b = p;
    if (dev == &b->hh && line == MC_HAMMERHEAD_LINE_MAP && state) remap(b);
    if (dev == &b->gc && line == MC_GC_LINE_INT) mc_ppc_set_interrupt(b->m, state);
    if (dev == &b->awacs && line == MC_AWACS_LINE_OUT_DRQ && state) mc_gc_dma_kick(&b->gc, 8);
}
static void pb_destroy(void *p) { mc_control_free(&((pboard *)p)->control); }
static void *pb_build(mc_ppc *m) {
    pboard *b = &pb_board;
    memset(b, 0, sizeof *b);
    b->m = m;
    const uint32_t banks[MC_HAMMERHEAD_BANKS] = {16u << 20, 16u << 20};
    mc_bandit_init(&b->bandit, &m->host, "bandit", MC_BANDIT_BANDIT, 0xF2000000u);
    mc_bandit_init(&b->chaos, &m->host, "chaos", MC_BANDIT_CHAOS, 0xF0000000u);
    mc_gc_init(&b->gc, &m->host, "gc");
    mc_hammerhead_init(&b->hh, &m->host, "hammerhead", 0x00000000u, MC_HAMMERHEAD_L2_NONE, banks);
    mc_nvram_init(&b->nvram, &m->host, "nvram", NULL);
    b->nvram.data[0x1308] = 3;
    mc_awacs_init(&b->awacs, &m->host, "awacs", NULL);
    mc_radacal_init(&b->dac, &m->host, "radacal", MC_DAC_RADACAL);
    if (!mc_control_init(&b->control, &m->host, "control", 4u << 20)) return NULL;
    mc_planb_init(&b->planb, &m->host, "planb");
    mc_via_init(&b->via, &m->host, "via", 9);
    const mc_pci_function gf = mc_gc_function(&b->gc), cf = mc_control_function(&b->control), pf = mc_planb_function(&b->planb);
    mc_bandit_attach(&b->bandit, MC_GC_DEVICE, &gf);
    mc_bandit_attach(&b->chaos, MC_CONTROL_DEVICE, &cf);
    mc_bandit_attach(&b->chaos, MC_PLANB_DEVICE, &pf);
    mc_gc_attach(&b->gc, MC_GC_AWACS, MC_AWACS_SIZE, 0, &b->awacs, &mc_awacs_ops);
    mc_gc_attach(&b->gc, MC_GC_VIA, 0x2000, 0, &b->via, &mc_via_ops);  /* Cuda's VIA, no Cuda yet */
    mc_gc_attach(&b->gc, MC_GC_DAC, 0x1000, 4, &b->dac, &mc_radacal_ops);
    mc_gc_attach(&b->gc, MC_NVRAM_PAGE_OFFSET, MC_NVRAM_WINDOW, 0, &b->nvram, &mc_nvram_page_ops);
    mc_gc_attach(&b->gc, MC_NVRAM_DATA_OFFSET, MC_NVRAM_WINDOW, 0, &b->nvram, &mc_nvram_data_ops);
    const mc_dbdma_port out = {&b->awacs, NULL, mc_awacs_dma_write, NULL};
    mc_gc_dma_port(&b->gc, 8, &out);
    const mc_ppc_port direct = {MC_PPC_PORT_DIRECT, 4, 0, 0};
    if (!mc_ppc_attach(m, mc_ppc_region(m, "bandit"), 0, MC_BANDIT_SPACE, &b->bandit, &mc_bandit_ops, direct) ||
        !mc_ppc_attach(m, mc_ppc_region(m, "chaos"), 0, MC_BANDIT_SPACE, &b->chaos, &mc_bandit_ops, direct) ||
        !mc_ppc_attach(m, mc_ppc_region(m, "sysctl"), 0, MC_HAMMERHEAD_SIZE, &b->hh, &mc_hammerhead_ops, direct))
        return NULL;
    remap(b);
    return b;
}
static const mc_board_ops pb_ops = {.line = pb_line, .destroy = pb_destroy};
static const uint32_t pb_roms[] = {0x9630C68Bu, 0x96CD923Du, 0x960E4BE9u, 0x960FC647u, 0};
static const uint16_t pb_ram[] = {32, 0};
static const mc_region pb_map[] = {
    {"ram", 0x00000000u, 0x80000000u, 0, MC_REGION_RAM},
    {"chaos", 0xF0000000u, 0x02000000u, 0x01FFFFFFu, MC_REGION_IO},
    {"bandit", 0xF2000000u, 0x02000000u, 0x01FFFFFFu, MC_REGION_IO},
    {"sysctl", 0xF8000000u, 0x01000000u, 0x00FFFFFFu, MC_REGION_IO},
    {"rom", 0xFF000000u, 0x01000000u, 0, MC_REGION_ROM},
};
static const mc_ppc_desc pb_desc = {
    .key = "pci-test", .name = "PCI chips test board", .cpu = PPC_MODEL_604, .cpu_hz = 120000000u,
    .timebase_hz = 12500000u, .cpi = 1, .roms = pb_roms, .ram_mb = pb_ram, .map = pb_map,
    .nmap = sizeof pb_map / sizeof *pb_map, .unmapped = MC_PPC_UNMAPPED_STOP, .build = pb_build, .ops = &pb_ops,
};

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

static int probe(const char *path) {
    char buffer[512];
    const char *roms = getenv("LIVADIX_ROMS");
    if (!path) {
        if (!roms) { printf("SKIP: LIVADIX_ROMS is not set\n"); return 77; }
        snprintf(buffer, sizeof buffer, "%s/power-macintosh-7200.rom", roms);
        path = buffer;
    }
    FILE *f = fopen(path, "rb");
    if (!f) { printf("SKIP: %s is absent\n", path); return 77; }
    static uint8_t image[4u << 20];
    const size_t size = fread(image, 1, sizeof image, f);
    fclose(f);
    mc_config c;
    char error[256] = "";
    mc_config_defaults(&c);
    mc_config_set_ram_kb(&c, 32u << 10);
    mc_ppc *m = mc_ppc_create(&pb_desc, &c, image, size, error, sizeof error);
    if (!m) { printf("FAIL: %s\n", error); return 1; }
    pboard *b = m->board;
    mc_run_status st = MC_RUN_OK;
    /* POST's memory sizing ($FFF04060) ends by storing the total at NVRAM
     * $1044 and each bank's base and size at $1048 + 8 n ($FFF040E4)
     * (Open Firmware later rewrites that NVRAM: taken when it appears). */
    const uint8_t *nv = b->nvram.data;
    uint8_t sized[0x18] = {0};
    for (int i = 0; i < 4000 && st == MC_RUN_OK; i++) {
        st = mc_ppc_run(m, 100000, 0);
        if (!sized[3] && be32(nv + 0x1044)) memcpy(sized, nv + 0x1044, sizeof sized);
    }
    printf("%s after %llu instructions (pc %08X); POST sized %u MB (bank 1 at %08X, %u MB), %u remaps, GC mask %08X\n",
           st == MC_RUN_ERROR ? mc_ppc_error(m) : "no stop", (unsigned long long)m->cpu.instructions, m->cpu.pc,
           be32(sized) >> 20, be32(sized + 0x0C), be32(sized + 0x10) >> 20, b->remaps, b->gc.mask);
    CHECK(be32(sized) == (32u << 20));
    CHECK(be32(sized + 0x04) == 0 && be32(sized + 0x08) == (16u << 20));
    CHECK(be32(sized + 0x0C) == (16u << 20) && be32(sized + 0x10) == (16u << 20));
    CHECK(nv[0x1800] == 0x12 && nv[0x1801] == 0x75);            /* Open Firmware ran and set up its NVRAM part† */
    printf("Open Firmware: Bandit aborts %u, mask %08X; Chaos aborts %u; Control BARs %08X %08X command %04X; Plan B BAR %08X\n",
           b->bandit.aborts, mc_pci_config_read(&b->bandit.cfg, MC_BANDIT_ADDRESS_MASK, 4), b->chaos.aborts,
           mc_pci_config_base(&b->control.cfg, 1), mc_pci_config_base(&b->control.cfg, 2),
           mc_pci_config_read(&b->control.cfg, MC_PCI_COMMAND, 2), mc_pci_config_base(&b->planb.cfg, 0));
    CHECK(mc_pci_config_base(&b->gc.cfg, 0) == 0xF3000000u && (b->gc.cfg.reg[MC_PCI_COMMAND] & MC_PCI_CMD_MEMORY));
    CHECK(b->gc.mask & 0x80000000u);
    mc_ppc_destroy(m);
    printf("PCI probe: %u assertions, %u failures\n", assertions, failures);
    return failures ? 1 : 0;
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "probe")) return probe(argc > 2 ? argv[2] : NULL);
    pci_config();
    bandit();
    dbdma();
    grand_central();
    mesh();
    memory_controllers();
    sound_and_video();
    board_identity();
    printf("PCI chips: %u assertions, %u failures\n", assertions, failures);
    return failures ? 1 : 0;
}
