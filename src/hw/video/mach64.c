/* Livadix - ATI mach64 GT-B. Sources and register mapping in mach64.h. */
#include "mach64.h"
#include <stdlib.h>
#include <string.h>

enum {
    CRTC_H_TOTAL_DISP=0x000, CRTC_V_TOTAL_DISP=0x008, CRTC_VLINE=0x010,
    CRTC_OFF_PITCH=0x014, CRTC_INT_CNTL=0x018, CRTC_GEN_CNTL=0x01C,
    CUR_CLR0=0x060, CUR_CLR1=0x064, CUR_OFFSET=0x068, CUR_POSN=0x06C, CUR_OFF=0x070,
    GP_IO=0x078, CLOCK_CNTL=0x090, GEN_TEST_CNTL=0x0D0, MEM_CNTL=0x0B0, DAC_REGS=0x0C0, DAC_CNTL=0x0C4,
    CONFIG_CHIP_ID=0x0E0, DST_OFF_PITCH=0x100, DST_X=0x104, DST_Y=0x108,
    DST_Y_X=0x10C, DST_WIDTH=0x110, DST_HEIGHT=0x114, DST_HEIGHT_WIDTH=0x118,
    DST_X_WIDTH=0x11C, DST_BRES_LNTH=0x120, DST_BRES_ERR=0x124,
    DST_BRES_INC=0x128, DST_BRES_DEC=0x12C, DST_CNTL=0x130,
    SRC_OFF_PITCH=0x180, SRC_X=0x184,
    SRC_Y=0x188, SRC_Y_X=0x18C, SRC_WIDTH1=0x190, SRC_HEIGHT1=0x194,
    SRC_HEIGHT1_WIDTH1=0x198, SRC_CNTL=0x1B4, HOST_CNTL=0x240,
    PAT_REG0=0x280, PAT_REG1=0x284, PAT_CNTL=0x288,
    SC_LEFT=0x2A0, SC_RIGHT=0x2A4, SC_TOP=0x2AC, SC_BOTTOM=0x2B0,
    SC_LEFT_RIGHT=0x2A8, SC_TOP_BOTTOM=0x2B4, DP_BKGD_CLR=0x2C0,
    DP_FRGD_CLR=0x2C4, DP_WRITE_MASK=0x2C8, DP_PIX_WIDTH=0x2D0,
    DP_MIX=0x2D4, DP_SRC=0x2D8, CLR_CMP_CLR=0x300,
    CLR_CMP_MSK=0x304, CLR_CMP_CNTL=0x308, FIFO_STAT=0x310,
    CONTEXT_MASK=0x320, CONTEXT_LOAD_CNTL=0x32C, GUI_TRAJ_CNTL=0x330, GUI_STAT=0x338,
};
#define REG(a,r) ((a)->reg[(r)/4u])

static unsigned pixel_bits(unsigned code) {
    static const uint8_t bits[8]={1,4,8,16,16,24,32,0};
    return bits[code&7u];
}

static void update_irq(mc_mach64 *a) {
    const uint32_t interrupts=REG(a,CRTC_INT_CNTL);
    const uint8_t irq=((interrupts&2u) && (interrupts&4u)) || ((interrupts&8u) && (interrupts&16u));
    if(irq!=a->irq) { a->irq=irq;mc_dev_line(&a->dev,MC_MACH64_LINE_IRQ,irq); }
}

/* RRG-G02700 Appendix B: the reference divider, four feedback dividers
 * and two-bit post-dividers program the pixel clock from 14.31818 MHz. */
static uint64_t frame_ticks(const mc_mach64 *a) {
    const uint64_t horizontal=((REG(a,CRTC_H_TOTAL_DISP)&0x1FFu)+1u)*8u;
    const uint64_t vertical=(REG(a,CRTC_V_TOTAL_DISP)&0x7FFu)+1u;
    const unsigned clock=REG(a,CLOCK_CNTL)&3u;
    const unsigned reference=a->pll[2], feedback=a->pll[7u+clock];
    if(!reference || !feedback || horizontal<64 || vertical<32) return MC_C15M_HZ/60u;
    const unsigned post=1u<<((a->pll[6]>>(clock*2u))&3u);
    const uint64_t numerator=(uint64_t)MC_C15M_HZ*horizontal*vertical*reference*post;
    const uint64_t ticks=numerator/(28636360u*feedback);
    return ticks ? ticks : 1;
}

static uint64_t active_ticks(const mc_mach64 *a) {
    const uint32_t total=(REG(a,CRTC_V_TOTAL_DISP)&0x7FFu)+1u;
    const uint32_t display=((REG(a,CRTC_V_TOTAL_DISP)>>16)&0x7FFu)+1u;
    return display<total ? a->frame_ticks*display/total : a->frame_ticks*9u/10u;
}

static void schedule_next(mc_mach64 *a) {
    const uint64_t next=a->next_vline<a->next_vblank?a->next_vline:a->next_vblank;
    mc_dev_schedule(&a->dev,next);
}
static void schedule_line(mc_mach64 *a) {
    a->next_vline=UINT64_MAX;
    const unsigned total=(REG(a,CRTC_V_TOTAL_DISP)&0x7FFu)+1u;
    const unsigned line=REG(a,CRTC_VLINE)&0x7FFu;
    if((REG(a,CRTC_INT_CNTL)&8u) && line<total) {
        uint64_t ticks=a->frame_ticks*line/total;
        if(!ticks) ticks=1;
        a->next_vline=a->frame_start+ticks;
        if(a->next_vline<=mc_dev_now(&a->dev)) a->next_vline+=a->frame_ticks;
    }
}
static void schedule_frame(mc_mach64 *a) {
    a->frame_ticks=frame_ticks(a);
    a->frame_start=mc_dev_now(&a->dev);
    a->next_vblank=a->frame_start+active_ticks(a);
    a->vblank=0;
    schedule_line(a);
    schedule_next(a);
}

static void event(void *dev,uint64_t now) {
    mc_mach64 *a=dev;
    if(now>=a->next_vline) {
        REG(a,CRTC_INT_CNTL)|=16u;
        a->next_vline+=a->frame_ticks;
    }
    if(now>=a->next_vblank) {
        if(!a->vblank) {
            a->vblank=1;
            REG(a,CRTC_INT_CNTL)|=4u;
            a->next_vblank=a->frame_start+a->frame_ticks;
        } else {
            a->vblank=0;
            a->frame_start=a->next_vblank;
            a->frame_ticks=frame_ticks(a);
            a->next_vblank=a->frame_start+active_ticks(a);
            schedule_line(a);
        }
    }
    update_irq(a);
    if(a->next_vblank<=now) a->next_vblank=now+1u;
    schedule_next(a);
}

static uint32_t pixel_read(const mc_mach64 *a,uint32_t offset_pitch,int x,int y,unsigned bits) {
    if(x<0 || y<0 || !bits) return 0;
    const uint64_t pixel=(uint64_t)(offset_pitch>>22)*8u*(unsigned)y+(unsigned)x;
    const uint64_t bit=(uint64_t)(offset_pitch&0xFFFFFu)*64u+pixel*bits;
    if(bit+bits>(uint64_t)a->vram_size*8u) return 0;
    const unsigned address=(unsigned)(bit>>3);
    if(bits<8) return a->vram[address]>>(8u-bits-(unsigned)(bit&7u))&((1u<<bits)-1u);
    uint32_t value=0;
    for(unsigned i=0;i<bits/8u;i++) value|=(uint32_t)a->vram[address+i]<<(i*8u);
    return value;
}

static void pixel_write(mc_mach64 *a,uint32_t offset_pitch,int x,int y,unsigned bits,uint32_t value) {
    if(x<0 || y<0 || !bits) return;
    const uint64_t pixel=(uint64_t)(offset_pitch>>22)*8u*(unsigned)y+(unsigned)x;
    const uint64_t bit=(uint64_t)(offset_pitch&0xFFFFFu)*64u+pixel*bits;
    if(bit+bits>(uint64_t)a->vram_size*8u) return;
    const unsigned address=(unsigned)(bit>>3);
    if(bits<8) {
        const unsigned shift=8u-bits-(unsigned)(bit&7u), mask=((1u<<bits)-1u)<<shift;
        a->vram[address]=(uint8_t)((a->vram[address]&~mask)|((value<<shift)&mask));
    } else for(unsigned i=0;i<bits/8u;i++) a->vram[address+i]=(uint8_t)(value>>(i*8u));
}

static uint32_t engine_read(const mc_mach64 *a, uint32_t pitch, int x, int y, unsigned bits) {
    if(bits && bits<8 && (REG(a,DP_PIX_WIDTH)&0x1000000u)) x^=(int)(8u/bits-1u);
    return pixel_read(a,pitch,x,y,bits);
}
static void engine_write(mc_mach64 *a, uint32_t pitch, int x, int y, unsigned bits, uint32_t value) {
    if(bits && bits<8 && (REG(a,DP_PIX_WIDTH)&0x1000000u)) x^=(int)(8u/bits-1u);
    pixel_write(a,pitch,x,y,bits,value);
}

/* ATI's sixteen DP_MIX functions, RRG-G02700 page 4-95. */
static uint32_t mix(unsigned function,uint32_t source,uint32_t destination) {
    switch(function&15u) {
    case 0:return ~destination; case 1:return 0; case 2:return UINT32_MAX; case 3:return destination;
    case 4:return ~source; case 5:return destination^source; case 6:return ~(destination^source); case 7:return source;
    case 8:return ~(destination&source); case 9:return destination|~source;
    case 10:return ~destination|source; case 11:return destination|source;
    case 12:return destination&source; case 13:return ~destination&source;
    case 14:return destination&~source; default:return ~(destination|source);
    }
}

static int coordinate(uint32_t value) { return (int)(value&0xFFFu)-(int)(value&0x1000u); }

/* RRG-G02700 ch.4, data path: a mono source selects independent foreground
 * and background color sources/mixes. The compare result inhibits writes. */
static uint32_t blit_pixel(mc_mach64 *a, unsigned column, unsigned row, unsigned bits) {
    if(!bits) return 0;
    unsigned sx = column, sy = row;
    const unsigned width = REG(a,SRC_WIDTH1) & 0x1FFFu;
    const unsigned height = REG(a,SRC_HEIGHT1) & 0x7FFFu;
    if (REG(a,SRC_CNTL) & 1u) {
        if (width) sx %= width;
        if (height) sy %= height;
    }
    if (REG(a,SRC_CNTL) & 4u) {
        unsigned linear = row * a->host_width + column;
        if ((REG(a,SRC_CNTL) & 8u) && bits < 8)
            linear = row * ((a->host_width * bits + 7u) / 8u * 8u / bits) + column;
        return engine_read(a,REG(a,SRC_OFF_PITCH), (int)linear, 0, bits);
    }
    const int dx = REG(a,DST_CNTL) & 1u ? 1 : -1;
    const int dy = REG(a,DST_CNTL) & 2u ? 1 : -1;
    return engine_read(a,REG(a,SRC_OFF_PITCH),coordinate(REG(a,SRC_X))+(int)sx*dx,
                      coordinate(REG(a,SRC_Y))+(int)sy*dy,bits);
}

static unsigned mono_pattern(const mc_mach64 *a, int x, int y) {
    const unsigned bit = ((unsigned)y & 7u) * 8u + ((unsigned)x & 7u);
    const unsigned shift = (REG(a,DP_PIX_WIDTH) & 0x1000000u) ? bit & 31u : (bit & 24u) + 7u - (bit & 7u);
    return (REG(a, bit < 32 ? PAT_REG0 : PAT_REG1) >> shift) & 1u;
}

static void draw_pixel(mc_mach64 *a, int x, int y, unsigned column, unsigned row, uint32_t host) {
    const unsigned bits=pixel_bits(REG(a,DP_PIX_WIDTH));
    if (!bits || x<(int)(REG(a,SC_LEFT_RIGHT)&0x1FFFu) || x>(int)((REG(a,SC_LEFT_RIGHT)>>16)&0x1FFFu) ||
        y<(int)(REG(a,SC_TOP_BOTTOM)&0x7FFFu) || y>(int)((REG(a,SC_TOP_BOTTOM)>>16)&0x7FFFu)) return;
    const unsigned mono_source=(REG(a,DP_SRC)>>16)&3u;
    unsigned mono=1;
    if(mono_source==1) mono=mono_pattern(a,x,y);
    if(mono_source==2) mono=host&1u;
    if(mono_source==3) mono=blit_pixel(a,column,row,1)&1u;
    const unsigned source=(REG(a,DP_SRC)>>(mono?8:0))&7u;
    uint32_t color=source==0?REG(a,DP_BKGD_CLR):REG(a,DP_FRGD_CLR);
    if(source==2) color=host;
    if(source==3) color=blit_pixel(a,column,row,pixel_bits(REG(a,DP_PIX_WIDTH)>>8));
    if(source==4) {
        const unsigned index=(REG(a,PAT_CNTL)&2u) ? ((unsigned)y&1u)*4u+((unsigned)x&3u) : ((unsigned)x&7u);
        color=REG(a,index<4?PAT_REG0:PAT_REG1)>>((index&3u)*8u)&255u;
    }
    const uint32_t old=engine_read(a,REG(a,DST_OFF_PITCH),x,y,bits);
    const unsigned compare=REG(a,CLR_CMP_CNTL)&7u;
    const uint32_t key=(REG(a,CLR_CMP_CNTL)&0x1000000u)?color:old;
    const int equal=(key&REG(a,CLR_CMP_MSK))==(REG(a,CLR_CMP_CLR)&REG(a,CLR_CMP_MSK));
    if(compare==1 || (compare==4 && !equal) || (compare==5 && equal)) return;
    const uint32_t result=mix(REG(a,DP_MIX)>>(mono?16:0),color,old), mask=REG(a,DP_WRITE_MASK);
    engine_write(a,REG(a,DST_OFF_PITCH),x,y,bits,(old&~mask)|(result&mask));
}

static int needs_host(const mc_mach64 *a) {
    const uint32_t sources=REG(a,DP_SRC);
    const unsigned mono=(sources>>16)&3u;
    return mono==2 || ((sources>>8)&7u)==2 || (mono!=0 && (sources&7u)==2);
}

static void advance_line(mc_mach64 *a) {
    const uint32_t control=REG(a,DST_CNTL);
    int x=coordinate(REG(a,DST_X)), y=coordinate(REG(a,DST_Y));
    int32_t error=(int32_t)(REG(a,DST_BRES_ERR)<<14)>>14;
    const int dx=control&1u?1:-1, dy=control&2u?1:-1;
    if(error>0 || (error==0 && !(control&0x800u))) {
        if(control&4u) x+=dx; else y+=dy;
        error+=(int32_t)(REG(a,DST_BRES_DEC)<<14)>>14;
    } else error+=(int32_t)(REG(a,DST_BRES_INC)<<14)>>14;
    if(control&4u) y+=dy; else x+=dx;
    REG(a,DST_X)=(uint32_t)x&0x1FFFu; REG(a,DST_Y)=(uint32_t)y&0x1FFFu;
    REG(a,DST_Y_X)=(REG(a,DST_X)<<16)|REG(a,DST_Y);
    REG(a,DST_BRES_ERR)=(uint32_t)error&0x3FFFFu;
}

/* PRG888GX0-01 section 2, Side Effects (p.2-43). */
static void rectangle_finish(mc_mach64 *a) {
    const uint32_t control=REG(a,DST_CNTL);
    if(control&8u) REG(a,DST_X)=(uint32_t)(coordinate(REG(a,DST_X))+(int)a->host_width*(control&1u?1:-1))&0x1FFFu;
    if(control&16u) REG(a,DST_Y)=(uint32_t)(coordinate(REG(a,DST_Y))+(int)a->host_height*(control&2u?1:-1))&0x1FFFu;
    REG(a,DST_Y_X)=(REG(a,DST_X)<<16)|REG(a,DST_Y);
}

static void emit_host_pixel(mc_mach64 *a, uint32_t value) {
    int x=coordinate(REG(a,DST_X)), y=coordinate(REG(a,DST_Y));
    if(!a->host_line) {
        x+=(int)a->host_column*(REG(a,DST_CNTL)&1u?1:-1);
        y+=(int)a->host_row*(REG(a,DST_CNTL)&2u?1:-1);
    }
    draw_pixel(a,x,y,a->host_column,a->host_row,value);
    if(a->host_line && (a->host_column+1u<a->host_width || !(REG(a,DST_CNTL)&32u))) advance_line(a);
    if(++a->host_column==a->host_width) {
        a->host_column=0;
        if(++a->host_row==a->host_height) { a->host_active=0;if(!a->host_line) rectangle_finish(a); }
    }
}

static void host_word(mc_mach64 *a, uint32_t value) {
    if(!a->host_active) return;
    const unsigned bits=((REG(a,DP_SRC)>>16)&3u)==2?1:pixel_bits(REG(a,DP_PIX_WIDTH)>>16);
    if(!bits) return;
    if(REG(a,HOST_CNTL)&2u) {
        if(bits==16) value=((value&0x00FF00FFu)<<8)|((value>>8)&0x00FF00FFu);
        if(bits==32) value=(value>>24)|((value>>8)&0xFF00u)|((value<<8)&0xFF0000u)|(value<<24);
    }
    if(bits<8 && !(REG(a,DP_PIX_WIDTH)&0x1000000u)) {
        uint32_t swapped=0;
        for(unsigned i=0;i<32;i+=bits) {
            const unsigned shift=(i&24u)+8u-bits-(i&7u);
            swapped|=((value>>shift)&((1u<<bits)-1u))<<i;
        }
        value=swapped;
    }
    if(!(REG(a,DST_CNTL)&1u) && !a->host_line && bits!=24) {
        uint32_t reversed=0;
        const uint32_t mask=bits==32?UINT32_MAX:(1u<<bits)-1u;
        for(unsigned i=0;i<32;i+=bits) reversed|=((value>>i)&mask)<<(32u-bits-i);
        value=reversed;
    }
    a->host_data|=(uint64_t)value<<a->host_bits;
    a->host_bits+=32;
    while(a->host_active && a->host_bits>=bits) {
        const uint32_t pixel=(uint32_t)a->host_data & (bits==32?UINT32_MAX:(1u<<bits)-1u);
        a->host_data>>=bits; a->host_bits-=bits;
        emit_host_pixel(a,pixel);
        if(!a->host_column && (REG(a,HOST_CNTL)&1u) && bits<8) {
            const unsigned skip=a->host_bits&7u;
            a->host_data>>=skip; a->host_bits-=skip;
        }
    }
    if(!a->host_active) a->host_data=a->host_bits=0;
}

static void rectangle(mc_mach64 *a) {
    if(!(REG(a,GEN_TEST_CNTL)&0x100u)) return;
    a->host_data=0; a->host_bits=a->host_column=a->host_row=0;
    a->host_width=REG(a,DST_WIDTH)&0x1FFFu; a->host_height=REG(a,DST_HEIGHT)&0x7FFFu;
    a->host_active=a->host_width && a->host_height && needs_host(a); a->host_line=0;
    if(!a->host_width || !a->host_height) return;
    a->draws++;
    if(a->host_active) return;
    const int x=coordinate(REG(a,DST_X)), y=coordinate(REG(a,DST_Y));
    const int dx=REG(a,DST_CNTL)&1u?1:-1, dy=REG(a,DST_CNTL)&2u?1:-1;
    for(unsigned row=0;row<a->host_height;row++) for(unsigned col=0;col<a->host_width;col++)
        draw_pixel(a,x+(int)col*dx,y+(int)row*dy,col,row,0);
    rectangle_finish(a);
}

static void bresenham(mc_mach64 *a) {
    if(!(REG(a,GEN_TEST_CNTL)&0x100u)) return;
    a->host_data=0; a->host_bits=a->host_column=a->host_row=0;
    a->host_width=REG(a,DST_BRES_LNTH)&0x7FFFu;
    if(a->host_width && !(REG(a,DST_CNTL)&32u)) a->host_width--;
    a->host_height=1; a->host_line=1;
    a->host_active=a->host_width && needs_host(a);
    if(!a->host_width) return;
    a->draws++;
    if(a->host_active) return;
    for(unsigned n=0;n<a->host_width;n++) {
        draw_pixel(a,coordinate(REG(a,DST_X)),coordinate(REG(a,DST_Y)),n,0,0);
        if(n+1u<a->host_width || !(REG(a,DST_CNTL)&32u)) advance_line(a);
    }
}

/* PRG888GX0-01 p.2-47, context layout; RRG-G02700 pp.4-102..105.
 * Buffer 0 is the final 256 bytes of VRAM. All contexts use PCI byte order.
 * Decode the composite aliases without triggering geometry while loading. */
static void composite_write(mc_mach64 *a, uint32_t value) {
    REG(a,DST_CNTL)=value&0xFFFFu;REG(a,SRC_CNTL)=value>>16&63u;
    REG(a,PAT_CNTL)=value>>24&7u;REG(a,HOST_CNTL)=value>>28&3u;
}
static uint32_t composite_read(const mc_mach64 *a) {
    return (REG(a,DST_CNTL)&0xFFFFu)|((REG(a,SRC_CNTL)&63u)<<16)|
        ((REG(a,PAT_CNTL)&7u)<<24)|((REG(a,HOST_CNTL)&3u)<<28);
}
static void context_load(mc_mach64 *a, uint32_t command) {
    static const uint16_t registers[] = {CONTEXT_MASK,0,DST_OFF_PITCH,DST_Y_X,DST_HEIGHT_WIDTH,
        DST_BRES_ERR,DST_BRES_INC,DST_BRES_DEC,SRC_OFF_PITCH,SRC_Y_X,SRC_HEIGHT1_WIDTH1,
        0x1A4,0x1B0,PAT_REG0,PAT_REG1,SC_LEFT_RIGHT,SC_TOP_BOTTOM,DP_BKGD_CLR,DP_FRGD_CLR,
        DP_WRITE_MASK,0x2CC,DP_PIX_WIDTH,DP_MIX,DP_SRC,CLR_CMP_CLR,CLR_CMP_MSK,CLR_CMP_CNTL,
        GUI_TRAJ_CNTL,CONTEXT_LOAD_CNTL};
    if(command&0x80000000u) return;
    for(unsigned hop=0;hop<a->vram_size/256u && (command&0x30000u);hop++) {
        const uint32_t pointer=command&0x3FFFu;
        if(pointer>=a->vram_size/256u) break;
        const uint32_t base=a->vram_size-(pointer+1u)*256u;
        uint32_t words[29];
        for(unsigned n=0;n<29;n++) {
            const uint8_t *p=a->vram+base+4u*n;
            words[n]=(uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;
        }
        REG(a,CONTEXT_MASK)=words[0];
        for(unsigned n=2;n<28;n++) if(words[0]&(1u<<n)) REG(a,registers[n])=words[n];
        if(words[0]&(1u<<3)) { REG(a,DST_Y)=REG(a,DST_Y_X)&0xFFFFu;REG(a,DST_X)=REG(a,DST_Y_X)>>16; }
        if(words[0]&(1u<<4)) { REG(a,DST_HEIGHT)=REG(a,DST_HEIGHT_WIDTH)&0xFFFFu;REG(a,DST_WIDTH)=REG(a,DST_HEIGHT_WIDTH)>>16; }
        if(words[0]&(1u<<9)) { REG(a,SRC_Y)=REG(a,SRC_Y_X)&0xFFFFu;REG(a,SRC_X)=REG(a,SRC_Y_X)>>16; }
        if(words[0]&(1u<<10)) { REG(a,SRC_HEIGHT1)=REG(a,SRC_HEIGHT1_WIDTH1)&0xFFFFu;REG(a,SRC_WIDTH1)=REG(a,SRC_HEIGHT1_WIDTH1)>>16; }
        if(words[0]&(1u<<27)) composite_write(a,words[27]);
        REG(a,DST_BRES_LNTH)=REG(a,DST_WIDTH);
        REG(a,CONTEXT_LOAD_CNTL)=words[28];
        const unsigned operation=(command>>16)&3u;
        if(operation==2) rectangle(a);
        if(operation==3) bresenham(a);
        command=words[28];
    }
}

static void written(mc_mach64 *a,unsigned offset) {
    const uint32_t value=REG(a,offset);
    if(offset>=0x200 && offset<0x240) { host_word(a,value); return; }
    switch(offset) {
    case DST_Y_X: REG(a,DST_Y)=value&0xFFFFu;REG(a,DST_X)=value>>16;break;
    case SRC_Y_X: REG(a,SRC_Y)=value&0xFFFFu;REG(a,SRC_X)=value>>16;break;
    case SRC_HEIGHT1_WIDTH1: REG(a,SRC_HEIGHT1)=value&0xFFFFu;REG(a,SRC_WIDTH1)=value>>16;break;
    case SC_LEFT: REG(a,SC_LEFT_RIGHT)=(REG(a,SC_LEFT_RIGHT)&0xFFFF0000u)|(value&0x1FFFu);break;
    case SC_RIGHT: REG(a,SC_LEFT_RIGHT)=(REG(a,SC_LEFT_RIGHT)&0xFFFFu)|((value&0x1FFFu)<<16);break;
    case SC_TOP: REG(a,SC_TOP_BOTTOM)=(REG(a,SC_TOP_BOTTOM)&0xFFFF0000u)|(value&0x7FFFu);break;
    case SC_BOTTOM: REG(a,SC_TOP_BOTTOM)=(REG(a,SC_TOP_BOTTOM)&0xFFFFu)|((value&0x7FFFu)<<16);break;
    case CONTEXT_LOAD_CNTL: context_load(a,value);break;
    case GUI_TRAJ_CNTL: composite_write(a,value);break;
    case 0x144: REG(a,DST_BRES_LNTH)=value; /* GT's line-length alias */
        /* fall through */
    case DST_BRES_LNTH: REG(a,DST_WIDTH)=value&0x7FFFu;if(!(value&0x80000000u)) bresenham(a);break;
    case DST_HEIGHT_WIDTH: REG(a,DST_HEIGHT)=value&0xFFFFu;REG(a,DST_WIDTH)=value>>16;rectangle(a);break;
    case DST_X_WIDTH: REG(a,DST_X)=value&0xFFFFu;REG(a,DST_WIDTH)=value>>16;rectangle(a);break;
    case DST_WIDTH: rectangle(a);break;
    default:break;
    }
}

/* RRG-G02700 GP_IO: data 0..15, direction 16..31. The original
 * GT-B driver uses pins 8, 12 and 13 for Apple sense lines 0, 1 and 2
 * (its native monitor routines at RAM $41AA4-$41C44). Standard and
 * extended sense codes follow the Performa 6400 note, Tables 3-13/14. */
static uint32_t gpio_read(const mc_mach64 *a) {
    static const unsigned pins[] = {8, 12, 13};
    const uint32_t value = REG(a, GP_IO);
    unsigned driven = 0;
    for (unsigned line = 0; line < 3; line++)
        if ((value & (1u << (pins[line] + 16u))) && !(value & (1u << pins[line]))) driven |= 1u << line;
    uint32_t result = value;
    for (unsigned line = 0; line < 3; line++) {
        if (value & (1u << (pins[line] + 16u))) continue;
        unsigned high = (a->monitor_code >> line) & 1u;
        for (unsigned low = 0; high && low < 3 && a->monitor_ext >= 0; low++) {
            if (!(driven & (1u << low)) || low == line) continue;
            const unsigned pair = (unsigned)a->monitor_ext >> (2u * low) & 3u;
            const unsigned bit = line < low ? line : line - 1u;
            if (!(pair & (1u << bit))) high = 0;
        }
        result = (result & ~(1u << pins[line])) | high << pins[line];
    }
    return result;
}
void mc_mach64_set_monitor(mc_mach64 *a, unsigned code, int extended) {
    a->monitor_code = (uint8_t)(code & 7u);
    a->monitor_ext = (int8_t)extended;
}

static uint8_t register_read(mc_mach64 *a,unsigned offset) {
    const unsigned reg=offset&~3u;
    if(offset==CLOCK_CNTL+2) return a->pll[(REG(a,CLOCK_CNTL)>>10)&63u];
    if(offset==DAC_REGS) return a->write_index;
    if(offset==DAC_REGS+2) return a->dac_mask;
    if(offset==DAC_REGS+3) return a->read_index;
    if(offset==DAC_REGS+1) {
        const uint8_t component=a->palette_rgb[a->read_index][a->read_component];
        const uint8_t value=(REG(a,DAC_CNTL)&0x100u)?(uint8_t)(component<<2):component;
        if(++a->read_component==3) { a->read_component=0;a->read_index++; }
        return value;
    }
    uint32_t value=REG(a,reg);
    if(reg==DST_Y_X) value=(REG(a,DST_X)<<16)|(REG(a,DST_Y)&0xFFFFu);
    if(reg==SRC_Y_X) value=(REG(a,SRC_X)<<16)|(REG(a,SRC_Y)&0xFFFFu);
    if(reg==DST_HEIGHT_WIDTH) value=(REG(a,DST_WIDTH)<<16)|(REG(a,DST_HEIGHT)&0xFFFFu);
    if(reg==SRC_HEIGHT1_WIDTH1) value=(REG(a,SRC_WIDTH1)<<16)|(REG(a,SRC_HEIGHT1)&0xFFFFu);
    if(reg==SC_LEFT) value=REG(a,SC_LEFT_RIGHT)&0x1FFFu;
    if(reg==SC_RIGHT) value=REG(a,SC_LEFT_RIGHT)>>16&0x1FFFu;
    if(reg==SC_TOP) value=REG(a,SC_TOP_BOTTOM)&0x7FFFu;
    if(reg==SC_BOTTOM) value=REG(a,SC_TOP_BOTTOM)>>16&0x7FFFu;
    if(reg==GP_IO) value=gpio_read(a);
    if(reg==GUI_TRAJ_CNTL) value=composite_read(a);
    if(reg==FIFO_STAT) value=0;
    if(reg==GUI_STAT) value=(uint32_t)a->host_active;
    if(reg==CRTC_INT_CNTL) value=(value&~1u)|a->vblank;
    if(reg==CRTC_VLINE) {
        const uint64_t elapsed=mc_dev_now(&a->dev)-a->frame_start;
        const uint32_t lines=(REG(a,CRTC_V_TOTAL_DISP)&0x7FFu)+1u;
        value=(value&0xFFFFu)|(uint32_t)(elapsed*lines/a->frame_ticks%lines)<<16;
    }
    return (uint8_t)(value>>(8u*(offset&3u)));
}

static void register_write(mc_mach64 *a,unsigned offset,uint8_t value) {
    const unsigned reg=offset&~3u, shift=8u*(offset&3u);
    if(offset==CLOCK_CNTL+2) {
        if(REG(a,CLOCK_CNTL)&0x200u) a->pll[(REG(a,CLOCK_CNTL)>>10)&63u]=value;
        return;
    }
    if(offset==DAC_REGS) { a->write_index=value;a->write_component=0;return; }
    if(offset==DAC_REGS+3) { a->read_index=value;a->read_component=0;return; }
    if(offset==DAC_REGS+2) { a->dac_mask=value;return; }
    if(offset==DAC_REGS+1) {
        a->palette_rgb[a->write_index][a->write_component]=(uint8_t)((REG(a,DAC_CNTL)&0x100u)?value>>2:value&63u);
        if(++a->write_component==3) {
            a->write_component=0;
            const uint8_t *rgb=a->palette_rgb[a->write_index];
            a->palette[a->write_index++]=0xFF000000u|(uint32_t)((rgb[0]<<2)|(rgb[0]>>4))<<16|
                (uint32_t)((rgb[1]<<2)|(rgb[1]>>4))<<8|(uint32_t)((rgb[2]<<2)|(rgb[2]>>4));
        }
        return;
    }
    if(reg==CONFIG_CHIP_ID || reg==FIFO_STAT || reg==GUI_STAT) return;
    if(reg==CRTC_INT_CNTL) {
        const uint32_t byte=(uint32_t)value<<shift, mask=255u<<shift;
        const uint32_t latched=REG(a,reg)&0x14u&~byte;
        REG(a,reg)=((REG(a,reg)&~mask)|(byte&mask))&~0x15u;
        REG(a,reg)|=latched;
        update_irq(a);
        schedule_line(a);schedule_next(a);
        return;
    }
    if(reg==GEN_TEST_CNTL && (offset&3u)==1u && !(value&1u)) {
        a->host_active=0;a->host_bits=0;a->host_data=0;
    }
    if(reg==DAC_CNTL && (offset&3u)==2u) value=(uint8_t)((value&~7u)|1u);
    REG(a,reg)=(REG(a,reg)&~(255u<<shift))|(uint32_t)value<<shift;
    if((offset&3u)==3u) {
        written(a,reg);
        if(reg==CRTC_VLINE) { schedule_line(a);schedule_next(a); }
        if(reg==CRTC_GEN_CNTL || reg==CRTC_V_TOTAL_DISP || reg==CRTC_H_TOTAL_DISP) schedule_frame(a);
    }
}

/* GT-B uses independent two-bit endian fields for each 8 MiB aperture
 * (RRG-G03300 MEM_CNTL, pp.4-17/18). Upper bits 26..27 are programmed
 * to 01 for 16-bit pixels and 10 for 32-bit pixels by the original driver. */
static unsigned memory_byte(const mc_mach64 *a,unsigned offset) {
    unsigned address=offset&0x7FFFFFu;
    const unsigned shift=(offset&0x800000u)?26u:24u;
    const unsigned code=(REG(a,MEM_CNTL)>>shift)&3u;
    if(code==1) address^=1u;
    if(code==2) address^=3u;
    return address;
}

static int memory_access(mc_mach64 *a,uint32_t address,unsigned size,uint32_t *value,int write) {
    uint32_t offset;
    const int registers=mc_pci_config_hit(&a->cfg,2,address,&offset);
    if(!registers && !mc_pci_config_hit(&a->cfg,0,address,&offset)) return 0;
    uint32_t result=0;
    for(unsigned i=0;i<size;i++) {
        const unsigned current=offset+i, aperture=current&0x7FFFFFu;
        uint8_t byte=(uint8_t)(*value>>(8u*i));
        if(registers || aperture>=0x7FF800u) {
            const unsigned reg=registers?(current&0x7FFu):((aperture-0x7FF800u)^0x400u);
            if(write) register_write(a,reg,byte); else byte=register_read(a,reg);
        } else {
            const unsigned physical=memory_byte(a,current);
            if(physical<a->vram_size) {
                if(write) a->vram[physical]=byte; else byte=a->vram[physical];
            } else byte=255;
        }
        result|=(uint32_t)byte<<(8u*i);
    }
    if(!write) *value=result;
    return 1;
}
static uint32_t config_read(void *dev,unsigned reg,unsigned size) { return mc_pci_config_read(&((mc_mach64 *)dev)->cfg,reg,size); }
static void config_write(void *dev,unsigned reg,unsigned size,uint32_t value) { mc_pci_config_write(&((mc_mach64 *)dev)->cfg,reg,size,value); }
static int memory_read(void *dev,uint32_t address,unsigned size,uint32_t *value) { *value=0;return memory_access(dev,address,size,value,0); }
static int memory_write(void *dev,uint32_t address,unsigned size,uint32_t value) { return memory_access(dev,address,size,&value,1); }
mc_pci_function mc_mach64_function(mc_mach64 *a) { return (mc_pci_function){a,config_read,config_write,memory_read,memory_write}; }

void mc_mach64_reset(mc_mach64 *a) {
    memset(a->reg,0,sizeof a->reg);
    memset(a->pll,0,sizeof a->pll);
    mc_pci_config_init(&a->cfg,0x1002,0x4754,0x9A,0x030000,0);
    mc_pci_config_bar(&a->cfg,0,MC_MACH64_APERTURE,0);
    a->cfg.reg[MC_PCI_BAR0]|=8u;
    mc_pci_config_bar(&a->cfg,1,0x100,1);
    mc_pci_config_bar(&a->cfg,2,0x1000,0);
    a->cfg.reg[MC_PCI_INT_PIN]=1;
    REG(a,CONFIG_CHIP_ID)=0x02004754u;
    /* RRG-G02700 DAC_CNTL: internal 24-bit palette (read-only type 1).
     * ROM 6E92FE08's GT-B driver preserves the power-on 8-bit selection
     * while configuring wait states and later DAC controls. */
    REG(a,DAC_CNTL)=0x00010100u;
    REG(a,MEM_CNTL)=a->vram_size==(4u<<20)?3u:2u;
    REG(a,DP_WRITE_MASK)=UINT32_MAX;
    REG(a,SC_LEFT_RIGHT)=0x1FFF0000u;
    REG(a,SC_TOP_BOTTOM)=0x7FFF0000u;
    a->read_index=a->write_index=a->read_component=a->write_component=0;
    a->dac_mask=255;
    a->host_active=a->host_line=0;
    a->host_data=0;a->host_bits=0;
    a->vblank=0;
    update_irq(a);
    schedule_frame(a);
}
static void reset(void *dev) { mc_mach64_reset(dev); }
const mc_dev_ops mc_mach64_ops={.event=event,.reset=reset};
int mc_mach64_init(mc_mach64 *a,const mc_hw_host *host,uint32_t vram_size) {
    memset(a,0,sizeof *a);
    if(vram_size!=(2u<<20) && vram_size!=(4u<<20)) return 0;
    a->dev=(mc_dev){&mc_mach64_ops,host,"ATI mach64 GT-B"};
    a->vram_size=vram_size;
    a->vram=calloc(1,vram_size);
    if(!a->vram) return 0;
    mc_mach64_set_monitor(a,6,-1);
    mc_mach64_reset(a);
    return 1;
}
void mc_mach64_free(mc_mach64 *a) { free(a->vram);free(a->scanout);a->vram=a->scanout=NULL; }

/* RRG ch.4 pp.4-33..37: a 64x64 two-bit cursor, least-significant
 * pixel first. Codes 0/1 select full DAC colors, 2 is transparent, 3 XORs. */
static void cursor_compose(mc_mach64 *a, const mc_mach64_view *view) {
    const unsigned left=REG(a,CUR_POSN)&0x7FFu, top=(REG(a,CUR_POSN)>>16)&0x7FFu;
    const unsigned ox=REG(a,CUR_OFF)&63u, oy=(REG(a,CUR_OFF)>>16)&63u;
    const uint64_t base=(uint64_t)(REG(a,CUR_OFFSET)&0xFFFFFu)*8u;
    for(unsigned y=0;y<64u-oy && top+y<view->height;y++)
        for(unsigned x=0;x<64u-ox && left+x<view->width;x++) {
            const unsigned pixel=(y+oy)*64u+x+ox;
            const uint64_t address=base+pixel/4u;
            if(address>=a->vram_size) continue;
            const unsigned code=(a->vram[address]>>((pixel&3u)*2u))&3u;
            if(code==2) continue;
            uint8_t *out=a->scanout+(size_t)(top+y)*view->row_bytes+(left+x)*4u;
            if(code==3) { out[1]^=255;out[2]^=255;out[3]^=255; }
            else {
                const uint32_t rgb=REG(a,code?CUR_CLR1:CUR_CLR0)>>8;
                out[0]=0;out[1]=(uint8_t)(rgb>>16);out[2]=(uint8_t)(rgb>>8);out[3]=(uint8_t)rgb;
            }
        }
}

int mc_mach64_view_get(mc_mach64 *a,mc_mach64_view *view) {
    const uint32_t control=REG(a,CRTC_GEN_CNTL), pitch=REG(a,CRTC_OFF_PITCH);
    const unsigned code=(control>>8)&7u, bits=pixel_bits(code);
    if(!(control&(1u<<25)) || (control&0x40u) || !code || !bits) return 0;
    view->width=(((REG(a,CRTC_H_TOTAL_DISP)>>16)&255u)+1u)*8u;
    view->height=((REG(a,CRTC_V_TOTAL_DISP)>>16)&2047u)+1u;
    const int cursor=(REG(a,GEN_TEST_CNTL)&0x80u)!=0;
    view->depth=cursor || bits==24?32u:bits;
    view->row_bytes=(pitch>>22)*bits;
    view->native_depth=bits; view->native_row_bytes=view->row_bytes;
    const uint32_t offset=(pitch&0xFFFFFu)*8u;
    if(view->row_bytes<(view->width*bits+7u)/8u || offset+(uint64_t)view->row_bytes*view->height>a->vram_size) return 0;
    view->pixels=a->vram+offset;
    view->palette=a->palette;
    if(bits<=8 && !cursor) {
        for(unsigned i=0;i<256;i++) a->masked_palette[i]=a->palette[i&a->dac_mask];
        view->palette=a->masked_palette;
        return 1;
    }
    const uint32_t row_bytes=view->width*(view->depth/8u), length=row_bytes*view->height;
    if(length>a->scanout_size) {
        uint8_t *next=realloc(a->scanout,length);
        if(!next) return 0;
        a->scanout=next;a->scanout_size=length;
    }
    for(unsigned y=0;y<view->height;y++) for(unsigned x=0;x<view->width;x++) {
        uint32_t pixel=pixel_read(a,pitch,(int)x,(int)y,bits);
        if(cursor) {
            if(bits<=8) pixel=a->palette[pixel&a->dac_mask]&0xFFFFFFu;
            else if(bits==16) {
                const unsigned r=code==4?pixel>>11&31u:pixel>>10&31u;
                const unsigned g=code==4?pixel>>5&63u:pixel>>5&31u, b=pixel&31u;
                pixel=((r*255u/31u)<<16)|((g*255u/(code==4?63u:31u))<<8)|(b*255u/31u);
            }
        } else if(code==4) pixel=((pixel>>1)&0x7FE0u)|(pixel&31u);
        uint8_t *out=a->scanout+(size_t)y*row_bytes+x*(view->depth/8u);
        for(unsigned i=0;i<view->depth/8u;i++) out[i]=(uint8_t)(pixel>>(8u*(view->depth/8u-i-1u)));
    }
    view->pixels=a->scanout;view->row_bytes=row_bytes;
    if(cursor) cursor_compose(a,view);
    return 1;
}
