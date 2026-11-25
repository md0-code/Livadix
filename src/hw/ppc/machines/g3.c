/* Livadix - Beige Power Macintosh G3 board.
 * Power Macintosh G3 Computers Developer Note (1998), ch.2 (Grackle,
 * Heathrow, Screamer, Cuda), ch.4 SDRAM; MPC106UM (1997) Table3-4 map B,
 * 3.2.6 memory banks; MPC750UM 2.1.2 time base. Device windows and PCI
 * function numbers: ROM 78F57389 POST FFF031DC-FFF03330 and Open Firmware
 * file 33B0AA-33C750. See chips/heathrow.h.
 * Original implementation using Livadix's existing chip interfaces. */
#include "../ppc_machine.h"
#include "../chips/grackle.h"
#include "../chips/ohci.h"
#include "../chips/heathrow.h"
#include "../chips/heathrow_ide.h"
#include "../chips/sdram_spd.h"
#include "../chips/bmac.h"
#include "../../livadix_card.h"
#include "../../guest/guest.h"
#include "../../sonic.h"
#include "../chips/screamer.h"
#include "../chips/awacs.h"
#include "../chips/mesh.h"
#include "../chips/swim3.h"
#include "../../via.h"
#include "../../adb.h"
#include "../../adbctl.h"
#include "../../pmu.h"
#include "../../scc.h"
#include "../../ata.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define MB (1024u*1024u)
#define CD_ID 3
typedef struct g3_board {
    mc_ppc *m; mc_grackle grackle; mc_heathrow io; mc_sdram_spd spd;
    mc_ohci usb;
    mc_via via; mc_adb_bus adb; mc_adb_keyboard keyboard; mc_adb_mouse mouse; mc_adbctl cuda;
    /* Portable board wiring; desktop Cuda and Livadix video stay separate. */
    int portable,pmu_resetting,slept;uint8_t sleep_ifr;
    mc_heathrow left_io;
    mc_spd_gpio spd_gpio[2];
    mc_pmu pmu;
    uint8_t nvram[8192]; char nvram_path[1024]; int64_t host_start;
    int powered,in_reset; mc_scc scc; mc_serial_host serial;
    mc_screamer codec; mc_awacs sound;
    mc_bmac bmac;mc_net_host net;mc_swim3 swim3;
    mc_swim3 bay_swim3;     /* the portable boards' second SWIM III, in Heathrow #2 */
    mc_scsi_bus scsi_bus;mc_mesh mesh;mc_scsi_unit unit[7];mc_vdisk *vdisk[7],*ata_vdisk,*ata_cd;
    mc_heathrow_ide ide[2];mc_livadix_card card;int card_fitted;uint32_t card_base;uint64_t fb_serial;

} g3_board;
static void map_ram(g3_board *g) {
    mc_mem_window w[MC_GRACKLE_BANKS]; const unsigned count=mc_grackle_windows(&g->grackle,w,MC_GRACKLE_BANKS);
    mc_bus_map_float(&g->m->bus,0,0x40000000u);
    for(unsigned k=0;k<count;++k)mc_bus_map_memory(&g->m->bus,w[k].base,w[k].size,g->m->ram+w[k].offset,w[k].bank_size,1);
}
/* MPC750UM 2.1.2.3, MPC750EC Table17: PLL_CFG straps, 66.667 MHz bus. */
static void cpu_straps(g3_board *g){g->m->cpu.hid[1]=g->m->rate/g->m->div>=250000000u?0xA0000000u:0xE0000000u;}
static void reset(void *board);
static void line(void *board,void *dev,int which,int state) {
    g3_board *g=board;
    if(dev==&g->grackle){if(which==MC_GRACKLE_LINE_MAP&&state)map_ram(g);}
    else if(dev==&g->io || (g->portable && dev==&g->left_io)){
        if(which==MC_HEATHROW_LINE_INT)mc_ppc_set_interrupt(g->m,g->io.irq || (g->portable && g->left_io.irq));
    }
    else if(dev==&g->via){if(which==MC_VIA_LINE_IRQ)mc_heathrow_set_source(&g->io,18,state);else if(which==MC_VIA_LINE_PB&&!g->portable)mc_adbctl_via_pb(&g->cuda,(uint8_t)state);}
    else if(g->portable && dev==&g->pmu) {
        if(which==MC_PMU_LINE_ACK) mc_via_set_pb(&g->via,state?0x08:0,0x08);
        else if(which==MC_PMU_LINE_INT) {
            /* The PMU clocks bytes separately; its CB1 interrupt must not
             * shift the VIA serial register a second time. */
            const uint8_t sr=g->via.sr,count=g->via.sr_count;
            mc_via_set_cb1(&g->via,state);
            g->via.sr=sr;g->via.sr_count=count;
        } else if(which==MC_PMU_LINE_POWER){
            /* As on the PBX boards (pbx.c): the PMU holds the CPU through
             * power-up and sleep, and switches the machine off. */
            if(state==MC_PMU_POWER_ON){g->powered=1;g->m->reset_held=g->pmu.reset!=0;}
            else if(state==MC_PMU_POWER_SLEEP){g->m->reset_held=1;g->slept=1;g->sleep_ifr=g->via.ifr;}
            else if(state==MC_PMU_POWER_OFF&&g->powered&&!g->pmu_resetting)mc_ppc_power_off(g->m);
        } else if(which==MC_PMU_LINE_RESET&&!g->pmu_resetting){
            g->m->reset_held=state!=0||mc_pmu_asleep(&g->pmu);
            /* Waking: the 750 loses power in sleep and starts from the
             * reset vector; memory, in self-refresh, keeps the
             * system's state, and both ROMs resume it from the vector the
             * system left at physical 0 (the 'Lars' signature at 4; Linux
             * arch/powerpc/platforms/powermac/sleep.S, Wallstreet). The
             * Grackle, refreshing the memory, keeps its configuration:
             * both ROMs (file $307F70) resume only when PICR1 no longer
             * reads its reset value $FF100010. */
            if(state&&g->slept){
                /* The bus clock stops in sleep: the VIA's timers and shift
                 * register raise nothing while it is off. */
                g->slept=0;mc_via_set_ifr(&g->via,(uint8_t)~g->sleep_ifr,0);
                ppc_reset(&g->m->cpu);ppc_jit_cpu_reset(g->m->jit);cpu_straps(g);
                /* A level the I/O still holds reaches the fresh processor. */
                mc_ppc_set_interrupt(g->m,g->io.irq||g->left_io.irq);
            }
        }
    }
    else if(!g->portable && dev==&g->cuda){
        if(which==MC_ADBCTL_LINE_NMI)mc_heathrow_set_source(&g->io,20,state);
        else if(which==MC_ADBCTL_LINE_POWER){if(state)g->powered=1;else if(g->powered)mc_ppc_power_off(g->m);}
        else if(which==MC_ADBCTL_LINE_RESET){if(state&&!g->in_reset){g->in_reset=1;reset(g);ppc_reset(&g->m->cpu);ppc_jit_cpu_reset(g->m->jit);cpu_straps(g);}else if(!state)g->in_reset=0;}
    } else if(dev==&g->sound){if(state)mc_heathrow_dma_kick(&g->io,which==MC_AWACS_LINE_OUT_DRQ?8:9);}
    else if(dev==&g->scc&&which==MC_SCC_LINE_IRQ)mc_heathrow_set_source(&g->io,15,state);
    else if(dev==&g->mesh){if(which==MC_MESH_LINE_IRQ)mc_heathrow_set_source(&g->io,12,state);else if(which==MC_MESH_LINE_DMA_STATUS)mc_heathrow_dma_bits(&g->io,0,0xE0,(uint8_t)state);else if(state)mc_heathrow_dma_kick(&g->io,0);}
    else if(dev==&g->swim3){if(which==MC_SWIM3_LINE_IRQ)mc_heathrow_set_source(&g->io,19,state);else if(state)mc_heathrow_dma_kick(&g->io,1);}
    else if(dev==&g->bay_swim3){if(which==MC_SWIM3_LINE_IRQ)mc_heathrow_set_source(&g->left_io,19,state);else if(state)mc_heathrow_dma_kick(&g->left_io,1);}
    else if(dev==&g->bmac){
        if(which==MC_BMAC_LINE_INT)mc_heathrow_set_source(&g->io,42,state);
        else if(which==MC_BMAC_LINE_RX&&state)mc_heathrow_dma_kick(&g->io,3);
        /* Apple BMacEnet hardware interface: Tx WaitSelect s5 holds
         * OUTPUT_LAST until the frame status is available. */
        else if(which==MC_BMAC_LINE_TX){mc_dbdma_set_bits(&g->io.ch[2],0x20,state?0x20:0);if(state)mc_heathrow_dma_kick(&g->io,2);}
    }
    else if(dev==&g->usb)mc_heathrow_set_source(&g->io,g->portable?26u:25u,state);
    else if(dev==&g->ide[0].ata)mc_heathrow_set_source(&g->io,13,state);
    else if(dev==&g->ide[1].ata)mc_heathrow_set_source(&g->io,14,state);
    else if(dev==&g->card&&which==MC_LIVADIX_CARD_LINE_IRQ)mc_heathrow_set_source(&g->io,g->portable?24u:23u,state);   /* the portable boards' video slot (PCI device 17) interrupts on source 24: Mac OS 9.0.4 moves the pointer at the card's vertical blanking only there */
}
static uint32_t pci_read(void *p,uint32_t off,unsigned size) {
    uint32_t value=0;mc_grackle_mem_read(&((g3_board *)p)->grackle,0x80000000u+off,size,&value);return value;
}
static void pci_write(void *p,uint32_t off,unsigned size,uint32_t value) {mc_grackle_mem_write(&((g3_board *)p)->grackle,0x80000000u+off,size,value);}
static int pci_ops_stable(void *dev, uint32_t offset, unsigned size) {
    g3_board *g = dev; uint32_t off;
    return mc_pci_config_hit(&g->io.cfg, 0, 0x80000000u + offset, &off) &&
           mc_heathrow_ops.stable(&g->io, off, size);
}
static const mc_dev_ops pci_ops={pci_read,pci_write,NULL,NULL,pci_ops_stable};
/* Grackle RCS1 bank (MPC106UM 1.3.4), board clock jumpers. The ROM
 * reads the BE word FF000004 (FFF033A4), bits3..1 select its clock
 * table at FFF033B4: revision4 code6 = 66.820000 MHz, TB = bus/4. Bit15 selects the
 * Macintosh I/O personality: FFF03290-FFF032D0 takes the PC FDC branch
 * when it is clear; FCode33C57C selects fdc vs swim3 from FCR bit27. */
static uint32_t straps_read(void *p,uint32_t off,unsigned size){(void)p;uint32_t v=0;for(unsigned k=0;k<size;++k)v=(v<<8)|((off+k==4)?0x80u:(off+k==5)?12u:0u);return v;}
static void straps_write(void *p,uint32_t off,unsigned size,uint32_t v){(void)p;(void)off;(void)size;(void)v;}
static const mc_dev_ops straps_ops={straps_read,straps_write,NULL,NULL,NULL};
static void missing(void *p,uint32_t off,unsigned size,int write,uint32_t value) {
    g3_board *g=p;mc_ppc *m=g->m;
    m->probe=(mc_ppc_probe){1,0xF3000000u+off,value,m->cpu.last_pc,size,write,0};
    snprintf(m->error,sizeof m->error,"Unimplemented Heathrow %s %u at %08X by %08X (%08X)",write?"write":"read",size,m->probe.address,m->probe.pc,value);
    m->failed=1;m->stop=1;
}
static uint32_t via_read(void *p,uint32_t off,unsigned size){g3_board *g=p;if(!g->portable)mc_adbctl_sync(&g->cuda);return mc_via_ops.read(&g->via,off,size);}
static void via_write(void *p,uint32_t off,unsigned size,uint32_t value){
    g3_board *g=p;
    if(!g->portable)mc_adbctl_sync(&g->cuda);
    mc_via_ops.write(&g->via,off,size,value);
    if(g->portable && ((off>>9)&15u)==0)mc_pmu_set_req(&g->pmu,(g->via.orb&0x10u)!=0);
}
static uint8_t pmu_shift(void *p,uint8_t value){return mc_via_shift8(p,value);}
static const mc_dev_ops via_ops={via_read,via_write,NULL,NULL,NULL};
/* OF file33C66C: direct 8 KiB NVRAM at +60000, a byte per16 addresses. */
static uint32_t nv_read(void *p,uint32_t off,unsigned size){(void)size;return ((g3_board *)p)->nvram[(off>>4)&8191u];}
static void nv_write(void *p,uint32_t off,unsigned size,uint32_t value){(void)size;((g3_board *)p)->nvram[(off>>4)&8191u]=(uint8_t)value;}
static const mc_dev_ops nv_ops={nv_read,nv_write,NULL,NULL,NULL};
static uint32_t scc_offset(uint32_t off){static const uint8_t order[]={0,4,2,6};return order[(off>>4)&3u];}
static uint32_t scc_read(void *p,uint32_t off,unsigned size){return mc_scc_ops.read(p,scc_offset(off),size);}
static void scc_write(void *p,uint32_t off,unsigned size,uint32_t value){mc_scc_ops.write(p,scc_offset(off),size,value);}
static const mc_dev_ops scc_ops={scc_read,scc_write,NULL,NULL,NULL};
static uint32_t audio_rate(void *p){return mc_awacs_rate(&((g3_board *)p)->sound);}
static size_t audio_pull(void *p,int16_t *stereo,size_t n){
    g3_board *g=p;n=mc_awacs_pull(&g->sound,stereo,n);mc_screamer_output(&g->codec,stereo,n);
    mc_ppc_audio_startup(g->m,stereo,n,g->sound.played,!g->sound.out_n&&!g->sound.ring_n&&!(g->io.ch[8].status&MC_DBDMA_ACTIVE));
    return n;
}
static int key(void *p,uint8_t code,int down){
    g3_board *g=p;
    /* PowerBook G3 Series manual: any key but Caps Lock wakes it; a
     * release does not. */
    if(g->portable&&mc_pmu_asleep(&g->pmu)){if(down&&code!=0x39)mc_pmu_wake(&g->pmu);}
    if(!g->portable&&code==0x7F)mc_adbctl_power_key(&g->cuda,down);
    int ok=mc_adb_keyboard_key(&g->keyboard,code,down);if(!g->portable)mc_adbctl_poke(&g->cuda);return ok;}
static int absolute_ready(void *p){return mc_ohci_pointer_ready(&((g3_board *)p)->usb);}
static int absolute_mouse(void *p,int x,int y,int button){g3_board *g=p;return mc_ohci_pointer(&g->usb,x,y,g->card.width,g->card.height,button);}
static int mouse(void *p,int dx,int dy,int button){g3_board *g=p;mc_adb_mouse_move(&g->mouse,dx,dy,button);if(!g->portable)mc_adbctl_poke(&g->cuda);return 1;}
/* The portable boards' PRAM file: the PMU's record (pmu.h: the clock), then
 * the 8 KB NVRAM the PMU serves, which holds the parameter RAM. */
static void set_clock(void *p,int64_t seconds){
    g3_board *g=p;g->host_start=seconds;
    if(!g->portable){mc_adbctl_set_clock(&g->cuda,seconds);return;}
    if(g->nvram_path[0]&&mc_pmu_nvram_load(&g->pmu,g->nvram_path,seconds)){
        FILE *f=fopen(g->nvram_path,"rb");
        if(f){if(fseek(f,8+1+8+256,SEEK_SET)||fread(g->nvram,1,sizeof g->nvram,f)!=sizeof g->nvram)memset(g->nvram,0,sizeof g->nvram);fclose(f);}
    }else mc_pmu_set_seconds(&g->pmu,(uint32_t)seconds);
}
static const int disk_ids[] = {0, 1, 2, 4, 5, 6};

static mc_vdisk *make_vdisk_for(mc_disk *const *images, unsigned count, int atapi, char *error, size_t capacity);
static mc_vdisk *make_vdisk(mc_disk *const *images, unsigned count, char *error, size_t capacity) {
    return make_vdisk_for(images, count, 0, error, capacity);
}
static mc_vdisk *make_vdisk_for(mc_disk *const *images, unsigned count, int atapi, char *error, size_t capacity) {
    /* An all-zero device is uninitialized hardware for Drive Setup. Its
     * block0/map/driver writes must survive closing and reopening it. */
    if(count==1){uint8_t first[4096];int blank=mc_disk_read_bytes(images[0],0,sizeof first,first);for(unsigned k=0;blank&&k<sizeof first;++k)blank=first[k]==0;if(blank)return mc_vdisk_create_device(images[0],error,capacity);}
    size_t size = 0, ata_size = 0;
    const uint8_t *driver = mc_guest_livadixdisk(&size);
    const uint8_t *ata = mc_guest_livadixdisk_ata(&ata_size);
    /* A disc on the ATAPI drive: the map in 2048-byte blocks the ROM's
     * ATAPI boot code reads (vdisk.h). */
    const mc_vdisk_drivers drivers = {driver, size, ata, ata_size, atapi};
    return mc_vdisk_create(images, count, &drivers, error, capacity);
}

static int disk_on_ata(const g3_board *q, unsigned i) {
    return i < q->m->config.disk_count && q->m->config.disks[i].bus == MC_BUS_ATA;
}

static void drop_disks(g3_board *q) {
    for (int id = 0; id < 7; id++) {
        if (id == CD_ID || !q->vdisk[id]) continue;
        mc_scsi_bus_attach(&q->scsi_bus, id, NULL);
        mc_vdisk_destroy(q->vdisk[id]);
        q->vdisk[id] = NULL;
    }
    if (q->ata_vdisk) {
        mc_ata_attach_disk(&q->ide[0].ata, 0, NULL);
        mc_vdisk_destroy(q->ata_vdisk);
        q->ata_vdisk = NULL;
    }
}

/* The internal ATA Manager drives the master only. Additional ATA images
 * become its partitions, using Livadix's existing ATA virtual-disk driver. */
static int layout_ata(g3_board *q, char *error, size_t capacity) {
    mc_disk *group[MC_CONFIG_DISKS];
    unsigned count = 0;
    for (unsigned i = 0; i < MC_CONFIG_DISKS; i++) {
        mc_disk *disk = mc_media_get(&q->m->media, MC_MEDIA_DISK, i, NULL);
        if (disk && disk_on_ata(q, i)) group[count++] = disk;
    }
    if (!count) return 1;
    char problem[256];
    q->ata_vdisk = make_vdisk(group, count, problem, sizeof problem);
    if (!q->ata_vdisk) {
        if (error && capacity) snprintf(error, capacity, "ATA disk: %s", problem);
        return 0;
    }
    mc_ata_attach_disk(&q->ide[0].ata, 0, q->ata_vdisk);
    return 1;
}

static int layout_disks(g3_board *q, char *error, size_t capacity) {
    const mc_config *c = &q->m->config;
    mc_disk *group[7][MC_CONFIG_DISKS];
    unsigned count[7] = {0};
    int last = -1;
    drop_disks(q);
    for (unsigned i = 0; i < MC_CONFIG_DISKS; i++) {
        mc_disk *disk = mc_media_get(&q->m->media, MC_MEDIA_DISK, i, NULL);
        if (!disk || i >= c->disk_count || !c->disks[i].scsi_id_set || disk_on_ata(q, i)) continue;
        const int id = c->disks[i].scsi_id;
        if (id < 0 || id >= 7 || id == CD_ID) {
            if (error && capacity) snprintf(error, capacity, "SCSI disk ID %d is unavailable (CD-ROM uses ID 3)", id);
            return 0;
        }
        group[id][count[id]++] = disk;
    }
    for (unsigned i = 0; i < MC_CONFIG_DISKS; i++) {
        mc_disk *disk = mc_media_get(&q->m->media, MC_MEDIA_DISK, i, NULL);
        if (!disk || (i < c->disk_count && c->disks[i].scsi_id_set) || disk_on_ata(q, i)) continue;
        int id = -1;
        for (size_t k = 0; k < sizeof disk_ids / sizeof *disk_ids && id < 0; k++)
            if (!count[disk_ids[k]]) id = disk_ids[k];
        if (id < 0) id = last;
        if (id < 0) continue;
        group[id][count[id]++] = disk;
        last = id;
    }
    for (int id = 0; id < 7; id++) {
        if (!count[id]) continue;
        char problem[256];
        q->vdisk[id] = make_vdisk(group[id], count[id], problem, sizeof problem);
        if (!q->vdisk[id]) {
            if (error && capacity) snprintf(error, capacity, "SCSI ID %d: %s", id, problem);
            drop_disks(q);
            return 0;
        }
        mc_scsi_disk_init(&q->unit[id], q->vdisk[id]);
        mc_scsi_bus_attach(&q->scsi_bus, id, mc_scsi_unit_target(&q->unit[id]));
    }
    if (!layout_ata(q, error, capacity)) {
        drop_disks(q);
        return 0;
    }
    return 1;
}


static void cd_ejected(void *p,mc_scsi_unit *unit,mc_vdisk *vd){g3_board *g=p;(void)unit;mc_vdisk_destroy(vd);g->vdisk[CD_ID]=NULL;mc_media_eject(&g->m->media,MC_MEDIA_CDROM,0);}
static void ata_ejected(void *p,unsigned dev,mc_vdisk *vd){g3_board *g=p;(void)dev;mc_vdisk_destroy(vd);g->ata_cd=NULL;mc_media_eject(&g->m->media,MC_MEDIA_CDROM,0);}
static void poll(void *p);
static int insert(void *p,mc_media_kind kind,unsigned drive,mc_disk *disk,char *error,size_t cap){
 g3_board *g=p;if(kind==MC_MEDIA_DISK)return layout_disks(g,error,cap);
 if(kind==MC_MEDIA_FLOPPY)return mc_swim3_insert(&g->swim3,drive,disk,0,error,cap);
 if(kind!=MC_MEDIA_CDROM||drive){if(error&&cap)snprintf(error,cap,"Unsupported G3 media drive");return 0;}
 if(!disk){poll(g);return 1;}
 mc_vdisk *vd=make_vdisk_for(&disk,1,g->m->config.cdrom_bus==MC_BUS_ATA,error,cap);if(!vd)return 0;
 if(g->m->config.cdrom_bus==MC_BUS_ATA){mc_vdisk *old=mc_ata_cd_remove(&g->ide[1].ata,0);mc_vdisk_destroy(old);g->ata_cd=vd;return mc_ata_cd_insert(&g->ide[1].ata,0,vd);}
 mc_vdisk *old=mc_scsi_unit_remove(&g->unit[CD_ID]);mc_vdisk_destroy(old);g->vdisk[CD_ID]=vd;mc_scsi_cd_insert(&g->unit[CD_ID],vd);return 1;
}
static void poll(void *p){
 g3_board *g=p;int cd_gone=0;mc_disk *cd=mc_media_get(&g->m->media,MC_MEDIA_CDROM,0,&cd_gone);
 if(!cd||cd_gone){if(g->ata_cd){mc_vdisk_destroy(mc_ata_cd_remove(&g->ide[1].ata,0));g->ata_cd=NULL;}if(g->vdisk[CD_ID]){mc_vdisk_destroy(mc_scsi_unit_remove(&g->unit[CD_ID]));g->vdisk[CD_ID]=NULL;}}
 for(unsigned i=0;i<g->swim3.drives;++i){int ejected=0,gone=0;mc_disk *chip=mc_swim3_media(&g->swim3,i,&ejected);mc_disk *disk=mc_media_get(&g->m->media,MC_MEDIA_FLOPPY,i,&gone);if(chip&&(ejected||gone||chip!=disk)){mc_swim3_remove(&g->swim3,i);if(ejected&&chip==disk&&!gone)mc_media_eject(&g->m->media,MC_MEDIA_FLOPPY,i);}}
}
static int floppy_read(void *p,uint8_t *b){return mc_swim3_dma_read(p,b)?MC_DBDMA_BYTE:MC_DBDMA_NONE;}
static int floppy_write(void *p,uint8_t b){return mc_swim3_dma_write(p,b);}
/* BMAC asserts DBDMA s6 at the receive record boundary (Apple BMacEnet
 * hardware interface, _resetChip: Rx BranchSelect/InterruptSelect s6). */
static int ethernet_read(void *p,uint8_t *byte){
 g3_board *g=p;int r=mc_bmac_dma_read(&g->bmac,byte);
 mc_dbdma_set_bits(&g->io.ch[3],0x40,r==MC_DBDMA_END?0x40:0);
 return r;
}
static void map_card(g3_board *g){
 uint32_t base=g->card_fitted&&mc_livadix_card_pci_decoding(&g->card)?mc_pci_config_base(&g->card.cfg,0):0;
 if(base==g->card_base)return;
 if(g->card_base)mc_bus_remap(&g->m->bus,mc_ppc_region(g->m,"pci"),g->card_base,g->card.vram_size);
 g->card_base=base;if(base)mc_bus_map_memory(&g->m->bus,base,g->card.vram_size,g->card.vram,g->card.vram_size,1);
}
static void card_cfg_write(void *p,unsigned r,unsigned n,uint32_t v){g3_board *g=p;mc_livadix_card_function(&g->card).config_write(&g->card,r,n,v);map_card(g);}
static uint32_t card_cfg_read(void *p,unsigned r,unsigned n){return mc_livadix_card_function(&((g3_board *)p)->card).config_read(&((g3_board *)p)->card,r,n);}
static int card_read(void *p,uint32_t a,unsigned n,uint32_t *v){return mc_livadix_card_function(&((g3_board *)p)->card).mem_read(&((g3_board *)p)->card,a,n,v);}
static int card_write(void *p,uint32_t a,unsigned n,uint32_t v){return mc_livadix_card_function(&((g3_board *)p)->card).mem_write(&((g3_board *)p)->card,a,n,v);}
static int framebuffer(void *p,mc_framebuffer *fb){g3_board *g=p;mc_livadix_card_view v;if(!g->card_fitted||(g->portable&&mc_pmu_asleep(&g->pmu))||!mc_livadix_card_view_get(&g->card,&v))return 0;fb->width=v.width;fb->height=v.height;fb->depth=v.depth;fb->row_bytes=v.row_bytes;fb->pixels=v.pixels;fb->palette=v.palette;fb->serial=++g->fb_serial;return 1;}
static uint32_t card_blits(void *p){return ((g3_board *)p)->card.blt.count;}
static void serial_tx(void *p,int ch,uint8_t byte){g3_board *g=p;if(g->serial.transmit)g->serial.transmit(g->serial.ctx,ch==MC_SCC_A?0u:1u,byte);}
static void serial_outputs(void *p,int ch,unsigned v){g3_board *g=p;if(g->serial.outputs)g->serial.outputs(g->serial.ctx,ch==MC_SCC_A?0u:1u,(v&MC_SCC_OUT_DTR)!=0);}
static void serial_host(void *p,const mc_serial_host *host){g3_board *g=p;if(host)g->serial=*host;else memset(&g->serial,0,sizeof g->serial);}
static size_t serial_receive(void *p,unsigned port,const uint8_t *data,size_t n){g3_board *g=p;return port<2?mc_scc_receive(&g->scc,port?MC_SCC_B:MC_SCC_A,data,n):n;}
static void serial_modem(void *p,unsigned port,int dcd,int cts){g3_board *g=p;if(port<2){mc_scc_set_dcd(&g->scc,port?MC_SCC_B:MC_SCC_A,dcd);mc_scc_set_cts(&g->scc,port?MC_SCC_B:MC_SCC_A,cts);}}
static void net_tx(void *p,const uint8_t *data,size_t n){g3_board *g=p;if(g->net.transmit)g->net.transmit(g->net.ctx,data,n);}
static void net_host(void *p,const mc_net_host *host){g3_board *g=p;if(host)g->net=*host;else memset(&g->net,0,sizeof g->net);g->bmac.link=host!=NULL;}
static int net_receive(void *p,const uint8_t *data,size_t n){return mc_bmac_receive(&((g3_board *)p)->bmac,data,n);}
static void reset(void *board){
    g3_board *g=board;g->m->startup_sound_done=0;cpu_straps(g);mc_grackle_reset(&g->grackle);map_ram(g);mc_heathrow_reset(&g->io);
    mc_via_ops.reset(&g->via);mc_scc_ops.reset(&g->scc);mc_screamer_init(&g->codec,&g->m->host);mc_awacs_reset(&g->sound);
    mc_ohci_reset(&g->usb);
    mc_mesh_reset(&g->mesh);mc_swim3_ops.reset(&g->swim3);mc_bmac_reset(&g->bmac);mc_heathrow_ide_ops.reset(&g->ide[0]);mc_heathrow_ide_ops.reset(&g->ide[1]);
    if(g->card_fitted){mc_livadix_card_ops.reset(&g->card);map_card(g);}
    mc_via_set_pa(&g->via,0xFF,0xFF);mc_via_set_pb(&g->via,0xF7,0xF7);
    if(g->portable){
        mc_heathrow_reset(&g->left_io);mc_swim3_ops.reset(&g->bay_swim3);
        /* Both ROMs' 68k startup at $FFC0F94C reads the primary
         * Heathrow's GPIO bits 5-7 (PPC word $E0000000 after byte swap).
         * Strap $80 selects ProductInfo $3042, Gestalt machine type 312
         * (Wallstreet); $A0 selects $3046, type 314 (PDQ), as the guest's
         * Gestalt reports on each. */
        for(unsigned i=0;i<2;i++)
            mc_spd_gpio_init(&g->spd_gpio[i],g->spd.present[i]?g->spd.data[i]:NULL,i?0:g->m->desc==&mc_pdq_desc?0xA0:0x80);
        mc_via_set_pb(&g->via,0x18,0x18);
        /* The SCSI disk mode cable's sense: both ROMs' 'sdm ' routine
         * (file $2FFF9C) reads the primary Heathrow's feature control
         * register and takes bit $00200000 low for the cable attached,
         * which sends the start-up into SCSI disk mode ($FFC1E8C0,
         * $FFC22CB0). No cable: the pin reads high. */
        g->io.feature_in=0x00200000u;
    }
}
static void save_nvram(g3_board *g){
    if(!g->nvram_path[0])return;
    if(g->portable){if(!mc_pmu_nvram_save(&g->pmu,g->nvram_path,g->host_start+(int64_t)(mc_ppc_now(g->m)/MC_C15M_HZ)))return;}
    else if(!mc_adbctl_nvram_save(&g->cuda,g->nvram_path,g->host_start))return;
    FILE *f=fopen(g->nvram_path,"ab");if(f){fwrite(g->nvram,1,sizeof g->nvram,f);fclose(f);}}
static int load_nvram(g3_board *g){if(!g->nvram_path[0]||!mc_adbctl_nvram_load(&g->cuda,g->nvram_path))return 0;FILE *f=fopen(g->nvram_path,"rb");if(!f)return 0;int ok=fseek(f,8+1+8+256,SEEK_SET)==0&&fread(g->nvram,1,sizeof g->nvram,f)==sizeof g->nvram;fclose(f);return ok;}
static void destroy(void *p){g3_board *g=p;save_nvram(g);drop_disks(g);mc_vdisk_destroy(g->vdisk[CD_ID]);mc_vdisk_destroy(g->ata_cd);mc_scsi_bus_free(&g->scsi_bus);if(g->card_fitted)mc_livadix_card_free(&g->card);if(g->portable)mc_pmu_free(&g->pmu);else free(g->cuda.mcu);free(g);}
static void *build(mc_ppc *m){
    g3_board *g=calloc(1,sizeof *g);if(!g)return NULL;g->m=m;m->board=g;
    const mc_hw_host *host=&m->host;const mc_ppc_port direct={MC_PPC_PORT_DIRECT,4,0,0};
    g->portable=m->desc==&mc_wallstreet_desc || m->desc==&mc_pdq_desc;
    /* PowerBook G3 Series Developer Note ch.4, Tables 4-2/4-5: the
     * right bay accepts an auto-eject floppy or an ATA module. Reuse
     * storage.cdrom bus: ata to select the CD module, as on the 190/1400.
     * The left bay's controller remains present without a drive. */
    if(g->portable) m->media.floppy_drives=m->config.cdrom_bus==MC_BUS_ATA?0u:m->media.floppy_drives?1u:0u;
    uint32_t banks[MC_GRACKLE_BANKS];
    const int ram_ok=g->portable?mc_sdram_spd_portable_init(&g->spd,m->ram_size,banks):mc_sdram_spd_init(&g->spd,m->ram_size,banks);
    if(!ram_ok){free(g);return NULL;}
    mc_grackle_init(&g->grackle,host,"Grackle",0x40,banks);
    mc_ppc_attach(m,mc_ppc_region(m,"grackle"),0,MC_GRACKLE_CONFIG_SIZE,&g->grackle,&mc_grackle_ops,direct);map_ram(g);
    mc_ppc_attach(m,mc_ppc_region(m,"pci"),0,0x7F000000u,g,&pci_ops,direct);
    mc_ppc_attach(m,mc_ppc_region(m,"straps"),0,0x00800000u,g,&straps_ops,direct);
    /* MPC106UM ch.7: PCI address and data phases take at least two
     * 33-MHz PCI clocks; round up to the C15M bus-time unit. The POST's
     * bounded VIA polls include these bus transfers, not just CPU clocks. */
    m->bus.region[mc_ppc_region(m,"pci")].cost=1;
    m->bus.region[mc_ppc_region(m,"grackle")].cost=1;
    mc_heathrow_init(&g->io,host,"Heathrow");g->io.unmapped_ctx=g;g->io.unmapped=missing;
    const mc_pci_function fn=mc_heathrow_function(&g->io);mc_grackle_attach(&g->grackle,16,&fn);
    /* Livadix virtual PCI expansion: OHCI at device 15. The original OF
     * slot-C interrupt property is 25 on desktop G3s, 26 on portables.
     * B46FFB63 ROM observation: the PCI@F interrupts cell is 0000001A.
     * All six Grackle G3 models share it; no installed guest driver is needed. */
    mc_ohci_init(&g->usb,host);const mc_pci_function usb=mc_ohci_function(&g->usb);mc_grackle_attach(&g->grackle,15,&usb);
    mc_bmac_init(&g->bmac,host,mc_sonic_default_address);g->bmac.port=g;g->bmac.transmit=net_tx;
    mc_heathrow_attach(&g->io,0x11000,MC_BMAC_SIZE,0,&g->bmac,&mc_bmac_ops);
    const mc_dbdma_port tx={&g->bmac,NULL,mc_bmac_dma_write,NULL,mc_bmac_dma_end},rx={g,ethernet_read,NULL,NULL,NULL};mc_heathrow_dma_port(&g->io,2,&tx);mc_heathrow_dma_port(&g->io,3,&rx);
    mc_scsi_bus_init(&g->scsi_bus);mc_mesh_init(&g->mesh,host,"MESH",&g->scsi_bus,0xE2);
    mc_heathrow_attach(&g->io,0x10000,0x100,4,&g->mesh,&mc_mesh_ops);
    const mc_dbdma_port mp={&g->mesh,mc_mesh_dma_read,mc_mesh_dma_write,NULL,NULL};mc_heathrow_dma_port(&g->io,0,&mp);
    mc_scsi_cd_init(&g->unit[CD_ID]);mc_scsi_unit_on_eject(&g->unit[CD_ID],cd_ejected,g);mc_scsi_bus_attach(&g->scsi_bus,CD_ID,mc_scsi_unit_target(&g->unit[CD_ID]));
    mc_swim3_init(&g->swim3,host,"SWIM III",g->portable?m->media.floppy_drives:1u);mc_swim3_set_stride(&g->swim3,4);mc_heathrow_attach(&g->io,0x15000,0x100,0,&g->swim3,&mc_swim3_ops);
    const mc_dbdma_port fp={&g->swim3,floppy_read,floppy_write,NULL,NULL};mc_heathrow_dma_port(&g->io,1,&fp);
    for(unsigned k=0;k<2;++k){mc_heathrow_ide_init(&g->ide[k],host);mc_heathrow_attach(&g->io,0x20000+0x1000*k,0x1000,0,&g->ide[k],&mc_heathrow_ide_ops);}
    if(!g->portable || m->config.cdrom_bus==MC_BUS_ATA){mc_ata_attach_cd(&g->ide[1].ata,0);mc_ata_on_eject(&g->ide[1].ata,ata_ejected,g);}
    if(g->portable){
        /* PowerBook G3 Series Developer Note ch. 2. POST configures the
         * second Heathrow at PCI device 13, MMIO $F4000000. */
        mc_heathrow_init(&g->left_io,host,"Heathrow #2");
        const mc_pci_function left=mc_heathrow_function(&g->left_io);
        mc_grackle_attach(&g->grackle,13,&left);
        mc_heathrow_attach(&g->io,0x34,4,0,&g->spd_gpio[0],&mc_spd_gpio_ops);
        mc_heathrow_attach(&g->left_io,0x34,4,0,&g->spd_gpio[1],&mc_spd_gpio_ops);
        /* Its SWIM III, without a drive (no floppy module in the
         * expansion bay): the ROM's floppy driver opens only when the
         * registers of both read back ($FFCFC2AA, $FFCFC300; the second at
         * Heathrow #2 + $15000 when $2400 bit 7 is set). A driver that
         * fails to open leaves its sleep task queued in freed memory, and
         * the task's wake call writes through SonyVars = -1 into $163. */
        mc_swim3_init(&g->bay_swim3,host,"SWIM III #2",0);mc_swim3_set_stride(&g->bay_swim3,4);
        mc_heathrow_attach(&g->left_io,0x15000,0x100,0,&g->bay_swim3,&mc_swim3_ops);
        const mc_dbdma_port bp={&g->bay_swim3,floppy_read,floppy_write,NULL,NULL};mc_heathrow_dma_port(&g->left_io,1,&bp);
    }
    {
        size_t rom_size=0;const uint8_t *card_rom=mc_guest_livadixcard_pci(&rom_size);int mode=mc_livadix_card_mode(m->config.width,m->config.height);
        if(mode<0||!mc_livadix_card_init(&g->card,host,"Livadix Display Card",MC_LIVADIX_CARD_VRAM,(unsigned)mode,card_rom,rom_size,m->config.video_acceleration)){destroy(g);return NULL;}
        g->card_fitted=1;mc_livadix_card_set_depth(&g->card,m->config.depth);
        const mc_pci_function card={g,card_cfg_read,card_cfg_write,card_read,card_write};mc_grackle_attach(&g->grackle,g->portable?17u:13u,&card);
    }
    mc_via_init(&g->via,host,"VIA",9);mc_heathrow_attach(&g->io,0x16000,0x2000,0,g,&via_ops);
    if(!g->portable)mc_heathrow_attach(&g->io,0x60000,0x20000,0,g,&nv_ops);
    mc_scc_init(&g->scc,host);mc_scc_set_clocks(&g->scc,MC_C15M_HZ/2u,MC_SCC_MAC_CLOCK_HZ,MC_SCC_MAC_CLOCK_HZ);
    const mc_scc_port sport={g,serial_tx,serial_outputs};mc_scc_set_port(&g->scc,&sport);
    mc_heathrow_attach(&g->io,0x13000,0x1000,0,&g->scc,&scc_ops);
    mc_heathrow_attach(&g->io,0x12000,0x1000,0,&g->scc,&mc_scc_ops);
    mc_screamer_init(&g->codec,host);
    const mc_awacs_codec codec={&g->codec,mc_screamer_control,mc_screamer_status};mc_awacs_init(&g->sound,host,"Heathrow sound",&codec);
    mc_heathrow_attach(&g->io,0x14000,0x1000,0,&g->sound,&mc_awacs_ops);
    const mc_dbdma_port out={&g->sound,NULL,mc_awacs_dma_write,NULL,NULL},in={&g->sound,mc_awacs_dma_read,NULL,NULL,NULL};
    mc_heathrow_dma_port(&g->io,8,&out);mc_heathrow_dma_port(&g->io,9,&in);
    mc_adb_bus_init(&g->adb);mc_adb_keyboard_init(&g->keyboard);mc_adb_mouse_init(&g->mouse);mc_adb_attach(&g->adb,&g->keyboard.dev);mc_adb_attach(&g->adb,&g->mouse.dev);
    if(g->portable){
        /* ROM $FFF075C0-$FFF07A28 drives PMU REQ/ACK on VIA PB4/PB3
         * and uses the VIA shift register for the bytes. */
        mc_pmu_init(&g->pmu,host,"PowerBook PMU",MC_PMU_99,MC_PMU_SHIFT,&g->adb,NULL,0);
        mc_pmu_set_shift(&g->pmu,pmu_shift,&g->via);
        mc_pmu_set_nvram(&g->pmu,g->nvram,sizeof g->nvram);
        mc_pmu_set_smart_battery(&g->pmu);
        if(!mc_config_nvram_path(&m->config,g->nvram_path,sizeof g->nvram_path))g->nvram_path[0]=0;
        /* Both ROMs' PMgrOp loads the send and reply count tables at
         * $FFC12AAE/$FFC12BAE. These also describe the POST NVRAM commands. */
        if(m->rom_size>=0x12AAEu+512u)mc_pmu_set_counts(&g->pmu,m->rom+0x12AAEu);
        /* A cold board starts the PMU's housekeeping (its autopoll of the
         * ADB devices), which mc_pmu_init alone leaves unscheduled. */
        g->pmu_resetting=1;mc_pmu_ops.reset(&g->pmu);g->pmu_resetting=0;
        m->reset_held=g->pmu.reset!=0;
        return g;
    }
    mc_adbctl_init(&g->cuda,host,"Cuda",MC_ADBCTL_CUDA,&g->via,&g->adb);mc_adbctl_set_part(&g->cuda,"341S0788");
    /* The supplied early Cuda dumps predate G3's combined-IIC command.
     * No matching G3 controller dump is indexed; auto uses the protocol
     * controller, as for any board whose matching firmware is absent. */
    if(m->config.controller==MC_CONTROLLER_FIRMWARE){snprintf(m->error,sizeof m->error,"No matching G3 Cuda firmware dump is available; use controller auto or protocol");destroy(g);return NULL;}
    if(!mc_config_nvram_path(&m->config,g->nvram_path,sizeof g->nvram_path))g->nvram_path[0]=0;
    if(!load_nvram(g))mc_adbctl_set_seconds(&g->cuda,0);
    /* Startup Disk PRAM (IM: OS Utilities, Start Manager); Livadix's SCSI
     * block driver uses refNum -(33+target), as on the other PPC boards. */
    if(m->config.boot_priority==MC_BOOT_CDROM&&m->config.cdrom_bus!=MC_BUS_ATA){
        uint8_t *pram=mc_adbctl_pram(&g->cuda);const uint16_t ref=(uint16_t)-(33+CD_ID);
        pram[0x78]=pram[0x79]=0;pram[0x7A]=(uint8_t)(ref>>8);pram[0x7B]=(uint8_t)ref;
    }
    g->cuda.iic_ctx=&g->spd;g->cuda.iic_transfer=mc_sdram_spd_transfer;
    mc_adbctl_ops.reset(&g->cuda);return g;
}
static const mc_board_ops ops={.reset=reset,.line=line,.key=key,.mouse=mouse,.absolute_ready=absolute_ready,.absolute_mouse=absolute_mouse,.set_clock=set_clock,.destroy=destroy,.audio_rate=audio_rate,.audio_pull=audio_pull,.insert=insert,.poll=poll,.framebuffer=framebuffer,.serial_host=serial_host,.serial_receive=serial_receive,.serial_modem=serial_modem,.net_host=net_host,.net_receive=net_receive,.card_blits=card_blits};
static const uint32_t roms[]={0x78F57389u,0x79D68D63u,0};
static const uint16_t ram[]={32,64,128,256,384,512,768,0};
static const char *const models[]={"power-macintosh-g3","power-macintosh-g3-v1","power-macintosh-g3-minitower","power-macintosh-g3-all-in-one",NULL};
static const mc_region map[]={
    {"ram",0,0x40000000u,0,MC_REGION_RAM},
    {"pci",0x80000000u,0x7F000000u,0x7FFFFFFFu,MC_REGION_IO},
    {"grackle",MC_GRACKLE_BASE,MC_GRACKLE_CONFIG_SIZE,0x003FFFFFu,MC_REGION_IO},
    {"straps",0xFF000000u,0x00800000u,0x007FFFFFu,MC_REGION_IO},
    {"rom",0xFF800000u,0x00800000u,0,MC_REGION_ROM},
};
static const mc_hw_caps caps={.card=1,.features=MC_FEATURE_ATA_DISK|MC_FEATURE_ATA_CD|MC_FEATURE_ENET};
const mc_ppc_desc mc_g3_desc={.key="g3",.models=models,.name="Power Macintosh G3",.cpu=PPC_MODEL_750,.cpu_hz=233000000u,.timebase_hz=16705000u,.cpi=1,.roms=roms,.ram_mb=ram,.map=map,.nmap=sizeof map/sizeof *map,.unmapped=MC_PPC_UNMAPPED_STOP,.build=build,.ops=&ops,.caps=&caps};

/* The PowerBook G3 Series (Wallstreet and PDQ): their screen is the PCI
 * Livadix Display Card (docs/NOTES.md); the right media bay takes a
 * floppy module or the ATAPI CD module. */
static const mc_hw_caps portable_caps={.card=1,.features=MC_FEATURE_ATA_DISK|MC_FEATURE_ATA_CD|MC_FEATURE_ENET|MC_FEATURE_MEDIA_BAY|MC_FEATURE_ONE_FLOPPY};
static const uint16_t portable_ram[]={32,64,96,128,160,192,0};
static const uint32_t wallstreet_roms[]={0xCBB01212u,0},pdq_roms[]={0xB46FFB63u,0};
static const char *const wallstreet_models[]={"powerbook-g3-wallstreet",NULL},*const pdq_models[]={"powerbook-g3-pdq",NULL};
const mc_ppc_desc mc_wallstreet_desc={
    .key="wallstreet",.models=wallstreet_models,.name="PowerBook G3 Series (Wallstreet)",
    .cpu=PPC_MODEL_750,.cpu_hz=233000000u,.timebase_hz=16705000u,.cpi=1,
    .roms=wallstreet_roms,.ram_mb=portable_ram,.map=map,.nmap=sizeof map/sizeof *map,
    .unmapped=MC_PPC_UNMAPPED_STOP,.build=build,.ops=&ops,.caps=&portable_caps,
};
const mc_ppc_desc mc_pdq_desc={
    .key="pdq",.models=pdq_models,.name="PowerBook G3 Series (PDQ)",
    .cpu=PPC_MODEL_750,.cpu_hz=266000000u,.timebase_hz=16705000u,.cpi=1,
    .roms=pdq_roms,.ram_mb=portable_ram,.map=map,.nmap=sizeof map/sizeof *map,
    .unmapped=MC_PPC_UNMAPPED_STOP,.build=build,.ops=&ops,.caps=&portable_caps,
};
