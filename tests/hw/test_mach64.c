/* ATI register/aperture fixture. Expected fields from RRG-G02700 ch.2/4/7. */
#include "hw_test.h"
#include "video/mach64.h"
#include <stdio.h>
#include <stdlib.h>
#define BASE 0x81000000u
#define MM (BASE+0x7FFC00u)
static mc_pci_function function;
static void write_reg(unsigned offset,uint32_t value) { CHECK(function.mem_write(function.dev,MM+offset,4,value)); }
static uint32_t read_reg(unsigned offset) { uint32_t value=0;CHECK(function.mem_read(function.dev,MM+offset,4,&value));return value; }
int main(void) {
    hw_fake fake;
    hw_fake_init(&fake);
    mc_mach64 *chip=calloc(1,sizeof *chip);
    CHECK(chip!=NULL);
    if(!chip)return 1;
    CHECK(mc_mach64_init(chip,&fake.host,2u<<20));
    function=mc_mach64_function(chip);
    CHECK_EQ(function.config_read(chip,0,4),0x47541002u);
    function.config_write(chip,0x10,4,UINT32_MAX);
    CHECK_EQ(function.config_read(chip,0x10,4),0xFF000008u);
    function.config_write(chip,0x10,4,BASE);
    uint32_t value=0;
    CHECK(!function.mem_read(chip,MM,4,&value));
    function.config_write(chip,4,2,2);
    CHECK_EQ(read_reg(0xE0),0x02004754u);
    write_reg(0xE0,0);CHECK_EQ(read_reg(0xE0),0x02004754u);
    write_reg(0xB0,1u<<26);
    CHECK(function.mem_write(chip,BASE+0x800000u,2,0x3412u));
    CHECK_EQ(chip->vram[0],0x34);CHECK_EQ(chip->vram[1],0x12);
    CHECK(function.mem_read(chip,BASE,2,&value));CHECK_EQ(value,0x1234u);
    write_reg(0xB0,2u<<26);
    CHECK(function.mem_write(chip,BASE+0x800004u,4,0x12345678u));
    CHECK_EQ(chip->vram[4],0x12);CHECK_EQ(chip->vram[7],0x78);
    write_reg(0xB0,(2u<<24)|(1u<<26));
    CHECK(function.mem_write(chip,BASE+8,4,0x11223344u));
    CHECK_EQ(chip->vram[8],0x11);CHECK_EQ(chip->vram[11],0x44);
    CHECK(function.mem_write(chip,BASE+0x800008u,2,0xABCDu));
    CHECK_EQ(chip->vram[8],0xAB);CHECK_EQ(chip->vram[9],0xCD);
    CHECK_EQ(read_reg(0xC4),0x10100u);
    write_reg(0xC4,7u<<16);
    CHECK_EQ(read_reg(0xC4),1u<<16); /* DAC type is read-only */
    CHECK(function.mem_write(chip,MM+0xC0,1,6));
    CHECK(function.mem_write(chip,MM+0xC1,1,63));
    CHECK(function.mem_write(chip,MM+0xC1,1,32));
    CHECK(function.mem_write(chip,MM+0xC1,1,15));
    CHECK_EQ(chip->palette[6],0xFFFF823Cu);
    CHECK(function.mem_write(chip,MM+0xC3,1,6));
    CHECK(function.mem_read(chip,MM+0xC1,1,&value));CHECK_EQ(value,63);
    CHECK(function.mem_read(chip,MM+0xC1,1,&value));CHECK_EQ(value,32);
    CHECK(function.mem_read(chip,MM+0xC1,1,&value));CHECK_EQ(value,15);
    write_reg(0xC4,0x100);
    CHECK(function.mem_write(chip,MM+0xC0,1,5));
    CHECK(function.mem_write(chip,MM+0xC1,1,255));
    CHECK(function.mem_write(chip,MM+0xC1,1,128));
    CHECK(function.mem_write(chip,MM+0xC1,1,0));
    CHECK_EQ(chip->palette[5],0xFFFF8200u);
    CHECK(function.mem_write(chip,MM+0xC3,1,5));
    CHECK(function.mem_read(chip,MM+0xC1,1,&value));CHECK_EQ(value,252);
    CHECK(function.mem_read(chip,MM+0xC1,1,&value));CHECK_EQ(value,128);
    CHECK(function.mem_read(chip,MM+0xC1,1,&value));CHECK_EQ(value,0);
    write_reg(0,99u|(79u<<16));write_reg(8,524u|(479u<<16));
    write_reg(0x14,80u<<22);write_reg(0x1C,0x03000200u);
    mc_mach64_view view;
    CHECK(mc_mach64_view_get(chip,&view));CHECK_EQ(view.width,640);CHECK_EQ(view.height,480);
    CHECK_EQ(view.depth,8);CHECK_EQ(view.row_bytes,640);
    write_reg(0x18,2);
    hw_fake_advance(&fake,hw_fake_deadline(&fake,chip));
    CHECK(read_reg(0x18)&1u);CHECK(read_reg(0x18)&4u);
    CHECK(hw_fake_line(&fake,chip,MC_MACH64_LINE_IRQ));
    write_reg(0x18,6);CHECK(!hw_fake_line(&fake,chip,MC_MACH64_LINE_IRQ));
    hw_fake_advance(&fake,hw_fake_deadline(&fake,chip));CHECK(!(read_reg(0x18)&1u));
    /* Vertical-line IRQ uses its own enable/status and write-one ack. */
    write_reg(0x10,200);write_reg(0x18,8);
    hw_fake_advance(&fake,chip->next_vline);
    CHECK(read_reg(0x18)&16u);CHECK(hw_fake_line(&fake,chip,MC_MACH64_LINE_IRQ));
    write_reg(0x18,24);CHECK(!hw_fake_line(&fake,chip,MC_MACH64_LINE_IRQ));
    write_reg(0x18,0);
    write_reg(0xD0,0x100);
    write_reg(0x100,80u<<22);write_reg(0x104,10);write_reg(0x108,4);
    write_reg(0x130,3);write_reg(0x2D0,0x00020202);write_reg(0x2D4,7u<<16);
    write_reg(0x2D8,1u<<8);write_reg(0x2C4,0x55);
    write_reg(0x118,(6u<<16)|3u);
    CHECK_EQ(chip->vram[4u*640u+10u],0x55);CHECK_EQ(chip->vram[6u*640u+15u],0x55);
    CHECK_EQ(chip->vram[6u*640u+16u],0);
    write_reg(0x180,80u<<22);write_reg(0x184,10);write_reg(0x188,4);
    write_reg(0x104,20);write_reg(0x108,10);write_reg(0x2D8,3u<<8);
    write_reg(0x118,(6u<<16)|3u);
    CHECK_EQ(chip->vram[10u*640u+20u],0x55);CHECK_EQ(chip->vram[12u*640u+25u],0x55);
    CHECK_EQ(chip->draws,2);
    /* RRG ch.4 GP_IO: released input levels and an extended VGA code. */
    mc_mach64_set_monitor(chip,6,-1);
    write_reg(0x78,0);
    CHECK_EQ(read_reg(0x78)&0x3100u,0x3000u);
    write_reg(0x78,1u<<29);CHECK_EQ(read_reg(0x78)&0x3100u,0x1000u);
    mc_mach64_set_monitor(chip,7,0x17);
    write_reg(0x78,0);CHECK_EQ(read_reg(0x78)&0x3100u,0x3100u);
    write_reg(0x78,1u<<29);CHECK_EQ(read_reg(0x78)&0x3100u,0x100u);
    /* Independent two-input truth tables exercise every DP_MIX function. */
    static const uint8_t truth[]={5,0,15,10,3,6,9,12,7,11,13,14,8,4,2,1};
    write_reg(0x104,0);write_reg(0x108,0);write_reg(0x114,1);write_reg(0x2D8,0x100);
    write_reg(0x2C4,0xCC);
    for(unsigned n=0;n<16;n++) {
        chip->vram[0]=0xAA;write_reg(0x2D4,n<<16);write_reg(0x110,1);
        CHECK_EQ(chip->vram[0],truth[n]*17u);
    }
    write_reg(0x2D4,7u<<16);write_reg(0x2C4,0x55);
    /* Host pixels remain pending until data arrives, then advance rows. */
    write_reg(0x2D8,2u<<8);write_reg(0x104,10);write_reg(0x108,16);
    write_reg(0x118,(4u<<16)|2u);CHECK_EQ(read_reg(0x338),1);
    write_reg(0x200,0x44332211);write_reg(0x23C,0x88776655);
    CHECK_EQ(read_reg(0x338),0);
    CHECK_EQ(chip->vram[16u*640u+10u],0x11);CHECK_EQ(chip->vram[17u*640u+13u],0x88);
    /* Packed host mono chooses independent foreground/background sources. */
    write_reg(0x2D8,0x020100);write_reg(0x2C0,0x33);write_reg(0x2D4,0x00070007);
    write_reg(0x108,18);write_reg(0x118,(8u<<16)|1u);write_reg(0x200,0xA5);
    static const uint8_t mono[]={0x55,0x33,0x55,0x33,0x33,0x55,0x33,0x55};
    for(unsigned n=0;n<8;n++) CHECK_EQ(chip->vram[18u*640u+10u+n],mono[n]);
    /* Fixed mono pattern and color-key transparency. */
    write_reg(0x2D8,0x010100);write_reg(0x280,0xAAAAAAAA);write_reg(0x284,0xAAAAAAAA);
    write_reg(0x104,0);write_reg(0x108,20);write_reg(0x118,(8u<<16)|1u);
    CHECK_EQ(chip->vram[20u*640u],0x55);CHECK_EQ(chip->vram[20u*640u+1],0x33);
    write_reg(0x2D8,0x100);write_reg(0x304,255);write_reg(0x300,0x55);
    write_reg(0x308,0x1000005);write_reg(0x110,1);
    CHECK_EQ(chip->vram[20u*640u],0x55);
    write_reg(0x2C4,0x99);write_reg(0x110,1);CHECK_EQ(chip->vram[20u*640u],0x99);
    write_reg(0x308,0);
    /* Bresenham X-major diagonal, with both positive and negative errors. */
    write_reg(0x104,30);write_reg(0x108,20);write_reg(0x130,35);
    write_reg(0x124,(uint32_t)-2);write_reg(0x128,4);write_reg(0x12C,(uint32_t)-4);
    write_reg(0x120,4);
    CHECK_EQ(chip->vram[20u*640u+30],0x99);CHECK_EQ(chip->vram[20u*640u+31],0x99);
    CHECK_EQ(chip->vram[21u*640u+32],0x99);CHECK_EQ(chip->vram[21u*640u+33],0x99);
    /* Partial host rows consume to the next byte only with HOST_BYTE_ALIGN. */
    write_reg(0x130,3);write_reg(0x104,0);write_reg(0x108,25);write_reg(0x2D8,0x020100);
    write_reg(0x240,1);write_reg(0x118,(3u<<16)|2u);write_reg(0x200,0x4080);
    CHECK_EQ(chip->vram[25u*640u],0x99);CHECK_EQ(chip->vram[25u*640u+1],0x33);
    CHECK_EQ(chip->vram[26u*640u],0x33);CHECK_EQ(chip->vram[26u*640u+1],0x99);
    write_reg(0x130,2);write_reg(0x104,3);write_reg(0x108,28);write_reg(0x2D8,0x200);
    write_reg(0x118,(4u<<16)|1u);write_reg(0x200,0x44332211);
    CHECK_EQ(chip->vram[28u*640u+3],0x44);CHECK_EQ(chip->vram[28u*640u],0x11);
    /* Context load uses the buffer's mask and launches one rectangle only
     * after the complete context has been restored. */
    const unsigned context=(2u<<20)-256u;
    for(unsigned n=0;n<256;n++) chip->vram[context+n]=0;
    uint32_t words[29]={0};
    words[0]=(1u<<2)|(1u<<3)|(1u<<4)|(1u<<18)|(1u<<21)|(1u<<22)|(1u<<23)|(1u<<27);
    words[2]=80u<<22;words[3]=(5u<<16)|30u;words[4]=(2u<<16)|2u;
    words[18]=0x42;words[21]=0x20202;words[22]=7u<<16;words[23]=0x100;words[27]=3;
    for(unsigned n=0;n<29;n++) for(unsigned j=0;j<4;j++) chip->vram[context+n*4u+j]=(uint8_t)(words[n]>>(j*8u));
    write_reg(0x32C,2u<<16);
    CHECK_EQ(chip->vram[30u*640u+5],0x42);CHECK_EQ(chip->vram[31u*640u+6],0x42);
    CHECK_EQ(read_reg(0x330),3);
    write_reg(0x130,27);write_reg(0x104,5);write_reg(0x108,35);write_reg(0x118,(2u<<16)|3u);
    CHECK_EQ(read_reg(0x104),7);CHECK_EQ(read_reg(0x108),38);
    /* Unselected background host data must not stall a solid foreground. */
    write_reg(0x130,3);write_reg(0x104,0);write_reg(0x108,40);write_reg(0x2D8,0x102);
    write_reg(0x2C4,0x66);write_reg(0x118,(2u<<16)|1u);
    CHECK_EQ(read_reg(0x338),0);CHECK_EQ(chip->vram[40u*640u],0x66);
    /* Packed host color-key comparison sees one pixel, not following data. */
    write_reg(0x2D8,0x200);write_reg(0x300,0xAB);write_reg(0x304,UINT32_MAX);write_reg(0x308,0x1000005);
    write_reg(0x118,(2u<<16)|1u);write_reg(0x200,0xCDAB);
    CHECK_EQ(chip->vram[40u*640u],0x66);CHECK_EQ(chip->vram[40u*640u+1],0xCD);
    write_reg(0x308,0);write_reg(0x184,12);write_reg(0x188,34);
    CHECK_EQ(read_reg(0x18C),(12u<<16)|34u);
    write_reg(0x2A8,(55u<<16)|4u);CHECK_EQ(read_reg(0x2A0),4);CHECK_EQ(read_reg(0x2A4),55);
    /* True-color scanout translates PCI little endian into Macintosh pixels. */
    write_reg(0x14,80u<<22);write_reg(0x1C,0x03000600u);
    chip->vram[0]=0x33;chip->vram[1]=0x22;chip->vram[2]=0x11;chip->vram[3]=0;
    CHECK(mc_mach64_view_get(chip,&view));CHECK_EQ(view.depth,32);
    CHECK_EQ(view.pixels[0],0);CHECK_EQ(view.pixels[1],0x11);CHECK_EQ(view.pixels[3],0x33);
    /* Hardware cursor colors bypass the desktop CLUT; transparent/XOR
     * pixels preserve/invert the scanout without changing guest VRAM. */
    const unsigned cursor_base=(2u<<20)-1024u;
    for(unsigned n=0;n<1024;n++) chip->vram[cursor_base+n]=0xAA;
    chip->vram[cursor_base]=0xE4; /* 0, 1, transparent, XOR */
    write_reg(0x60,0x12345600);write_reg(0x64,0xABCDEF00);
    write_reg(0x68,cursor_base/8u);write_reg(0x6C,0);write_reg(0x70,0);
    write_reg(0xD0,0x180);CHECK(mc_mach64_view_get(chip,&view));
    CHECK_EQ(view.pixels[1],0x12);CHECK_EQ(view.pixels[2],0x34);CHECK_EQ(view.pixels[3],0x56);
    CHECK_EQ(view.native_depth,32);CHECK_EQ(view.native_row_bytes,2560);
    CHECK_EQ(view.pixels[5],0xAB);CHECK_EQ(view.pixels[7],0xEF);
    CHECK_EQ(chip->vram[0],0x33); /* scanout is not written back */
    write_reg(0x1C,0x03000200u);CHECK(mc_mach64_view_get(chip,&view));
    CHECK_EQ(view.depth,32);CHECK_EQ(view.native_depth,8);CHECK_EQ(view.native_row_bytes,640);
    mc_mach64_free(chip);free(chip);
    printf("mach64: %d failures\n",mc_test_failures);
    return mc_test_failures?1:0;
}
