/* Livadix - MPC106 manual-derived fixtures (sources in grackle.h). */
#include "ppc/chips/grackle.h"
#include "ppc/ppc_machine.h"
#include "config.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
static unsigned checks, failures, maps;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); } } while (0)
static void line(void *ctx, void *dev, int which, int state) { (void)ctx; (void)dev; CHECK(which == MC_GRACKLE_LINE_MAP); if (state) ++maps; }
static const mc_hw_host host = {.line = line};
static uint32_t cfg_read(void *p, unsigned r, unsigned s) { return mc_pci_config_read(p, r, s); }
static void cfg_write(void *p, unsigned r, unsigned s, uint32_t v) { mc_pci_config_write(p, r, s, v); }
static uint32_t memory_value;
static int memory_read(void *p, uint32_t a, unsigned s, uint32_t *v) {
    uint32_t offset; (void)s;
    if (!mc_pci_config_hit(p, 0, a, &offset)) return 0;
    *v = memory_value; return 1;
}
static int memory_write(void *p, uint32_t a, unsigned s, uint32_t v) {
    uint32_t offset; (void)s;
    if (!mc_pci_config_hit(p, 0, a, &offset)) return 0;
    memory_value = v; return 1;
}
static void configuration(void) {
    mc_grackle g;
    mc_grackle_init(&g, &host, "Grackle", 0x40, NULL); /* fixture silicon revision */
    const mc_dev_ops *o = &mc_grackle_ops;
    CHECK(mc_grackle_config_read(&g,0,0,0,4) == 0x00021057u);
    CHECK(mc_grackle_config_read(&g,0,0,4,4) == 0x00800006u);
    CHECK(mc_grackle_config_read(&g,0,0,8,4) == 0x06000040u);
    CHECK(mc_grackle_config_read(&g,0,0,0xF0,4) == 0xFF820000u);
    CHECK(mc_grackle_config_read(&g,0,0,0xA8,4) == 0xFF100010u);
    o->write(&g,0,4,0x00000080u);
    CHECK(o->read(&g,0x100000,4) == 0x00000080u); /* address alias */
    CHECK(o->read(&g,MC_GRACKLE_CONFIG_DATA,4) == 0x57100200u);
    CHECK(o->read(&g,MC_GRACKLE_CONFIG_DATA,1) == 0x57);
    CHECK(o->read(&g,MC_GRACKLE_CONFIG_DATA+1,1) == 0x10);
    CHECK(o->read(&g,MC_GRACKLE_CONFIG_DATA+2,2) == 0x0200);
    o->write(&g,0,4,0x0C000080u);
    o->write(&g,MC_GRACKLE_CONFIG_DATA,4,0xFFFFFFFFu);
    CHECK(mc_grackle_config_read(&g,0,0,0x0C,4) == 8); /* read-only cache-line/latency/header */
    o->write(&g,0,4,0x04000080u);
    o->write(&g,MC_GRACKLE_CONFIG_DATA,2,0xFFFFu);
    CHECK(mc_grackle_config_read(&g,0,0,4,2) == 0x146);
    o->write(&g,0,4,0x00000880u); /* absent bus 8 */
    CHECK(o->read(&g,MC_GRACKLE_CONFIG_DATA,4) == 0xFFFFFFFFu);
    CHECK(mc_grackle_config_read(&g,0,0,6,2) & MC_PCI_ST_MASTER_ABORT);
    mc_grackle_config_write(&g,0,0,6,2,MC_PCI_ST_MASTER_ABORT);
    CHECK(!(mc_grackle_config_read(&g,0,0,6,2) & MC_PCI_ST_MASTER_ABORT));
    CHECK(mc_grackle_config_read(&g,0,1,0,4) == 0xFFFFFFFFu);
    mc_pci_config card;
    mc_pci_config_init(&card,0x1234,0x5678,1,0x030000,0);
    mc_pci_config_bar(&card,0,4096,0);
    const mc_pci_function f = {&card,cfg_read,cfg_write,memory_read,memory_write};
    CHECK(!mc_grackle_attach(&g,0,&f)); CHECK(mc_grackle_attach(&g,13,&f));
    CHECK(mc_grackle_config_read(&g,13,0,0,4) == 0x56781234u);
    mc_grackle_config_write(&g,13,0,0x10,4,0x90000000u);
    mc_grackle_config_write(&g,13,0,4,2,MC_PCI_CMD_MEMORY);
    memory_value = 0x11223344u;
    uint32_t v = 0;
    CHECK(mc_grackle_mem_read(&g,0x90000000u,4,&v) && v == 0x44332211u);
    CHECK(mc_grackle_mem_write(&g,0x90000000u,4,0xCAFEBABEu) && memory_value == 0xBEBAFECAu);
    CHECK(!mc_grackle_mem_read(&g,0x90001000u,4,&v) && v == 0xFFFFFFFFu);
    mc_grackle_config_write(&g,0,0,4,2,MC_PCI_CMD_MEMORY);
    CHECK(!mc_grackle_mem_read(&g,0x90000000u,4,&v));
    mc_grackle_reset(&g);
    CHECK(mc_grackle_config_read(&g,13,0,0,4) == 0x56781234u); /* attached hardware survives reset */
}
static void banks(void) {
    mc_grackle g;
    const uint32_t sizes[8] = {16u<<20,32u<<20};
    mc_grackle_init(&g,&host,"Grackle",0x40,sizes);
    mc_mem_window w[8];
    CHECK(!mc_grackle_windows(&g,w,8));
    /* Table 3-25: low byte describes bank 0, next byte bank 1. */
    mc_grackle_config_write(&g,0,0,MC_GRACKLE_START,4,0x00004000u);
    mc_grackle_config_write(&g,0,0,MC_GRACKLE_END,4,0x00007F1Fu);
    mc_grackle_config_write(&g,0,0,MC_GRACKLE_ENABLE,1,3);
    CHECK(!mc_grackle_windows(&g,w,8)); /* MEMGO still clear */
    mc_grackle_config_write(&g,0,0,MC_GRACKLE_MCCR1,4,MC_GRACKLE_MEMGO);
    CHECK(mc_grackle_windows(&g,w,8) == 2);
    CHECK(w[0].base == 0 && w[0].size == (32u<<20) && w[0].bank_size == (16u<<20));
    CHECK(w[1].base == (64u<<20) && w[1].size == (64u<<20) && w[1].offset == (16u<<20));
    unsigned bank; uint32_t offset;
    CHECK(mc_grackle_decode(&g,20u<<20,&bank,&offset) && bank == 0 && offset == (4u<<20));
    CHECK(mc_grackle_decode(&g,100u<<20,&bank,&offset) && bank == 1 && offset == (20u<<20));
    CHECK(!mc_grackle_decode(&g,32u<<20,&bank,&offset));
    CHECK(!mc_grackle_decode(&g,128u<<20,&bank,&offset));
    mc_grackle_config_write(&g,0,0,MC_GRACKLE_EXT_START,1,0xFF);
    mc_grackle_config_write(&g,0,0,MC_GRACKLE_EXT_END,1,0xFF);
    CHECK(mc_grackle_config_read(&g,0,0,MC_GRACKLE_EXT_START,1) == 3);
    CHECK(mc_grackle_decode(&g,0x30400000u,&bank,&offset) && bank == 0 && offset == (4u<<20));
    mc_grackle_config_write(&g,0,0,MC_GRACKLE_ENABLE,1,2);
    CHECK(!mc_grackle_decode(&g,0x30400000u,&bank,&offset));
    mc_grackle_reset(&g);
    CHECK(!mc_grackle_windows(&g,w,8));
    CHECK(g.bank_size[0] == (16u<<20) && g.bank_size[1] == (32u<<20));
    CHECK(maps > 0);
}
/* Real CPU bus cycles must distinguish FEC address from FEE data. A zero
 * board region mask had aliased these ports despite component tests passing. */
static void board_bus(void) {
    uint8_t *image=calloc(1,4u<<20);image[0]=0x78;image[1]=0xF5;image[2]=0x73;image[3]=0x89;mc_config c;char error[256]="";
    mc_config_defaults(&c);mc_config_set_ram_kb(&c,64u<<10);c.model=(uint32_t)mc_model_index(mc_model_find("power-macintosh-g3"));c.cpu_hz=mc_model_find("power-macintosh-g3")->cpu_hz;
    mc_ppc *m=mc_ppc_create(&mc_g3_desc,&c,image,4u<<20,error,sizeof error);free(image);
    CHECK(m!=NULL);if(!m)return;
    uint32_t v=0;
    CHECK(!mc_bus_write(&m->bus,0xFEC00000u,4,0x00000080u));
    CHECK(!mc_bus_read(&m->bus,0xFEE00000u,4,&v)&&v==0x57100200u);
    CHECK(!mc_bus_write(&m->bus,0xFEC00000u,4,0x00800080u));
    CHECK(!mc_bus_read(&m->bus,0xFEE00000u,4,&v)&&v==0x6B101000u);
    CHECK(!mc_bus_read(&m->bus,0xFF000004u,2,&v)&&v==0x800C);
    CHECK(m->cpu.hid[1]==0xA0000000u);
    mc_ppc_destroy(m);
}
/* Walk the actual Wallstreet PCI functions and bit-bang a random read.
 * The SPD and board configuration EEPROM addresses must remain distinct. */
static void i2c_drive(mc_ppc *m,uint32_t addr,int scl,int sda) {
    uint32_t value=(scl?0:0x10000u)|(sda?0:0x20000u);
    CHECK(!mc_bus_write(&m->bus,addr,4,mc_pci_swap(value,4)));
}
static unsigned i2c_sample(mc_ppc *m,uint32_t addr) {
    uint32_t v=0;
    CHECK(!mc_bus_read(&m->bus,addr,4,&v));
    return (mc_pci_swap(v,4)>>21)&1u;
}
static void i2c_start(mc_ppc *m,uint32_t addr) {
    i2c_drive(m,addr,0,1);
    i2c_drive(m,addr,1,1);
    i2c_drive(m,addr,1,0);
    i2c_drive(m,addr,0,0);
}
static void i2c_stop(mc_ppc *m,uint32_t addr) {
    i2c_drive(m,addr,0,0);
    i2c_drive(m,addr,1,0);
    i2c_drive(m,addr,1,1);
}
static unsigned i2c_byte(mc_ppc *m,uint32_t addr,unsigned byte) {
    for (unsigned bit=0x80;bit;bit>>=1) {
        int sda=(byte&bit)!=0;
        i2c_drive(m,addr,0,sda);
        i2c_drive(m,addr,1,sda);
        i2c_drive(m,addr,0,sda);
    }
    i2c_drive(m,addr,0,1);
    i2c_drive(m,addr,1,1);
    unsigned nack=i2c_sample(m,addr);
    i2c_drive(m,addr,0,1);
    return !nack;
}
static unsigned i2c_read(mc_ppc *m,uint32_t addr,int last) {
    unsigned value=0;
    for (unsigned bit=0;bit<8;++bit) {
        i2c_drive(m,addr,1,1);
        value=(value<<1)|i2c_sample(m,addr);
        i2c_drive(m,addr,0,1);
    }
    i2c_drive(m,addr,0,last);
    i2c_drive(m,addr,1,last);
    i2c_drive(m,addr,0,1);
    return value;
}
static void portable_spd(void) {
    const char *key="powerbook-g3-wallstreet";
    CHECK(mc_ppc_find(key)==&mc_wallstreet_desc);
    const mc_ppc_desc *desc=mc_ppc_find_board(key);
    CHECK(desc==&mc_wallstreet_desc);
    if(!desc)return;
    static const uint8_t rom[4096]={0xCB,0xB0,0x12,0x12};
    mc_config c;char error[256]="";
    mc_config_defaults(&c);mc_config_set_ram_kb(&c,160u<<10);
    mc_ppc *m=mc_ppc_create(desc,&c,rom,sizeof rom,error,sizeof error);
    CHECK(m!=NULL);if(!m)return;
    for(unsigned slot=0;slot<2;++slot) {
        const unsigned device=slot?13u:16u;
        const uint32_t base=slot?0xF4000000u:0xF3000000u, addr=base+0x34u;
        const uint32_t cfg=0x80000000u|(device<<11);
        CHECK(!mc_bus_write(&m->bus,0xFEC00000u,4,mc_pci_swap(cfg|0x10u,4)));
        CHECK(!mc_bus_write(&m->bus,0xFEE00000u,4,mc_pci_swap(base,4)));
        CHECK(!mc_bus_write(&m->bus,0xFEC00000u,4,mc_pci_swap(cfg|4u,4)));
        CHECK(!mc_bus_write(&m->bus,0xFEE00000u,2,mc_pci_swap(MC_PCI_CMD_MEMORY,2)));
        uint32_t gpio=0;
        CHECK(!mc_bus_read(&m->bus,addr,4,&gpio));
        CHECK((gpio&0xe0000000u)==(slot?0:0x80000000u));
        i2c_start(m,addr);
        CHECK(!i2c_byte(m,addr,0xae));
        i2c_stop(m,addr);
        i2c_start(m,addr);
        CHECK(i2c_byte(m,addr,0xa0));
        CHECK(i2c_byte(m,addr,2));
        i2c_start(m,addr);
        CHECK(i2c_byte(m,addr,0xa1));
        CHECK(i2c_read(m,addr,0)==4); /* SDRAM */
        CHECK(i2c_read(m,addr,0)==12);
        CHECK(i2c_read(m,addr,0)==(slot?9u:8u));
        CHECK(i2c_read(m,addr,1)==(slot?2u:1u));
        i2c_stop(m,addr);
    }
    mc_ppc_destroy(m);
}
static int probe(const char *path) {
    static uint8_t image[4u<<20];
    FILE *f = fopen(path,"rb");
    if (!f) { printf("SKIP: no %s\n",path); return 77; }
    const size_t size = fread(image,1,sizeof image,f); fclose(f);
    mc_config c; char error[256] = "";
    mc_config_defaults(&c); mc_config_set_ram_kb(&c,64u<<10); c.jit=1;c.model=(uint32_t)mc_model_index(mc_model_find("power-macintosh-g3"));c.cpu_hz=mc_model_find("power-macintosh-g3")->cpu_hz;
    mc_ppc *m = mc_ppc_create(&mc_g3_desc,&c,image,size,error,sizeof error);
    if (!m) { printf("FAIL: %s\n",error); return 1; }
    mc_run_status status = MC_RUN_OK;
    for (unsigned k=0;k<20 && status==MC_RUN_OK;++k) status=mc_ppc_run(m,100000,0);
    printf("G3 Grackle fixture: %llu instructions, pc %08X: %s\n",
           (unsigned long long)m->cpu.instructions,m->cpu.pc,status==MC_RUN_ERROR?mc_ppc_error(m):"instruction bound reached");
    printf("first unimplemented access: %s %u bytes at %08X by %08X, value %08X\n",
           m->probe.write?"write":"read",m->probe.size,m->probe.address,m->probe.pc,m->probe.value);
    /* With the actual board I/O mask, both ROMs must make progress through
     * real config/data cycles; the former zero-mask fixture aliased them. */
    uint32_t fcr=0;
    const int mac_io=!mc_bus_read(&m->bus,0xF3000038u,4,&fcr)&&!(fcr&8u);
    /* FCR bit27 in the little-endian I/O register is CPU bit3: it must
     * stay in Macintosh SWIM3 mode after the board-strap check. */
    const int ok = status == MC_RUN_OK && m->cpu.instructions > 325 && !m->probe.hit && mac_io;
    mc_ppc_destroy(m); return ok?0:1;
}
int main(int argc,char **argv) {
    if (argc==3 && !strcmp(argv[1],"probe")) return probe(argv[2]);
    configuration(); banks(); board_bus(); portable_spd(); printf("grackle: %u assertions, %u failures\n",checks,failures); return failures?1:0;
}
