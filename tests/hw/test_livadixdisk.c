/* Livadix's block driver (src/hw/guest/drivers/livadixdisk.s, and its ATA flavour
 * livadixdisk_ata.s) run on the phase-1 CPU in isolation: the image built into Livadix is loaded from the virtual
 * disk's driver partition and called as the Start Manager calls it (IM:
 * Devices 4-11), with the operating-system traps it uses answered by this
 * harness: _DrvrInstall, _HLock, _AddDrive, _PostEvent, _NewPtr and
 * _BlockMove (Inside Macintosh II and Devices chapter 1), the original SCSI
 * Manager's _SCSIDispatch
 * selectors over a real mc_scsi_bus with Livadix's targets (Devices chapter 3:
 * Pascal stack conventions, TIB opcodes) and _SCSIAtomic for the SCSI
 * Manager 4.3 API (Devices 4-21 to 4-30, 4-40 to 4-42, and Apple
 * Universal Interfaces 3.4 SCSI.h: register calling convention). Prime, Control and Status are then called with
 * parameter blocks as the Device Manager would.
 *
 * ATA flavour: the same harness answers _ATAManager ($AAF1) as Apple's ATA
 * Device Software Guide describes it (ataPBHdr, ataExecIO, ATA_DrvrRegister;
 * Pascal stack call): an ATA_ExecIO executes the parameter block's task file
 * and command on a real mc_ata (src/hw/ata.c), moving PIO data as the guide
 * says (ATA protocol by BlindTxSize bytes a DRQ, ATAPI protocol by the byte
 * count the device gives in the cylinder registers, the command packet first). */
#include "scsi_util.h"
#include "guest.h"
#include "m68k.h"
#include "scsi_target.h"
#include "ata.h"

#include <string.h>

#define RAM_SIZE 0x200000u
#define UNIT_TABLE 0x2000u
#define HEAP 0x4000u
#define IODONE 0x0E00u          /* JIODone points here: an RTS, counted */
#define SENTINEL 0x0E10u        /* return address of every call */
#define PB 0x3000u
#define BUFFER 0x40000u
#define DRIVER 0x10000u
#define DRIVER_CD 0x20000u

static uint8_t ram[RAM_SIZE];
static m68k_cpu cpu;
static mc_scsi_bus bus;
static mc_scsi_unit disk, cd;
static mc_disk *img1, *img2, *iso;
static mc_vdisk *vd, *vcd;
static uint32_t heap = HEAP;
static int exceptions, iodones, unknown_traps;
static unsigned events, event_msg[8];
static unsigned xref_calls, xref_function;
static int16_t xref_refnum;
static uint32_t xref_device;
static int scsi43_available, legacy_blocked;
static unsigned legacy_calls, scsi43_execs, virtual_queries, gestalt_calls;
static uint32_t scsi43_pb[8];
static hw_fake fk;
static mc_ata ata;
static unsigned ata_registers, ata_execs, ata_unknown;
static int16_t ata_reg_refnum;
static uint32_t ata_reg_device;
static int ata_fail;        /* ATA_ExecIO calls to fail (the device reports an error) */
static unsigned ata_cmds[256];

static uint32_t rd8(uint32_t a) { return a < RAM_SIZE ? ram[a] : 0; }
static uint32_t rd16(uint32_t a) { return rd8(a) << 8 | rd8(a + 1); }
static uint32_t rd32(uint32_t a) { return rd16(a) << 16 | rd16(a + 2); }
static void wr8(uint32_t a, uint32_t v) { if (a < RAM_SIZE) ram[a] = (uint8_t)v; }
static void wr16(uint32_t a, uint32_t v) { wr8(a, v >> 8); wr8(a + 1, v); }
static void wr32(uint32_t a, uint32_t v) { wr16(a, v >> 16); wr16(a + 2, v); }

static int bus_read(void *ctx, uint32_t a, unsigned size, unsigned fc, uint32_t *v) {
    (void)ctx; (void)fc;
    if (a + size > RAM_SIZE) return 1;
    *v = size == 1 ? rd8(a) : size == 2 ? rd16(a) : rd32(a);
    return 0;
}
static int bus_write(void *ctx, uint32_t a, unsigned size, unsigned fc, uint32_t v) {
    (void)ctx; (void)fc;
    if (a + size > RAM_SIZE) return 1;
    if (size == 1) wr8(a, v); else if (size == 2) wr16(a, v); else wr32(a, v);
    return 0;
}
static void on_exception(void *ctx, m68k_cpu *c, unsigned vector, uint32_t pc) {
    (void)ctx; (void)c;
    fprintf(stderr, "exception %u at %08X\n", vector, pc);
    exceptions++;
}

/* --- the operating-system traps -------------------------------------- */

static uint16_t pop16(void) { uint16_t v = (uint16_t)rd16(cpu.r[15]); cpu.r[15] += 2; return v; }
static uint32_t pop32(void) { uint32_t v = rd32(cpu.r[15]); cpu.r[15] += 4; return v; }
static void result(int16_t err) { wr16(cpu.r[15], (uint16_t)err); }

/* The old SCSI Manager over the bus: initiator 7, selection without ATN,
 * TIB scInc/scNoInc/scStop; error codes scCommErr 2, scPhaseErr 5,
 * scComplPhaseErr 10 (IM: Devices 3-44). */
static int16_t tib_transfer(uint32_t tib, int write) {
    for (;;) {
        uint16_t op = (uint16_t)rd16(tib);
        uint32_t p1 = rd32(tib + 2), p2 = rd32(tib + 6);
        if (op == 7) return 0;              /* scStop */
        if (op != 1 && op != 2) return 4;   /* only what the driver uses */
        for (uint32_t i = 0; i < p2; i++) {
            int phase = mc_scsi_bus_phase(&bus);
            if (write ? phase != MC_SCSI_DATA_OUT : phase != MC_SCSI_DATA_IN) return 5;
            if (write) mc_scsi_bus_out(&bus, (uint8_t)rd8(p1 + i));
            else wr8(p1 + i, mc_scsi_bus_in(&bus, 0));
        }
        if (op == 1) wr32(tib + 2, p1 + p2);
        tib += 10;
    }
}

static void scsi_dispatch(void) {
    uint16_t selector = pop16();
    legacy_calls++;
    if (legacy_blocked && selector == 1) { result(8); return; }
    switch (selector) {
    case 1:     /* SCSIGet */
        result(mc_scsi_bus_arbitrate(&bus, 7) ? 0 : 7);
        break;
    case 2: {   /* SCSISelect(targetID) */
        uint16_t id = pop16();
        result(mc_scsi_bus_select(&bus, id, 0) ? 0 : 2);
        break;
    }
    case 3: {   /* SCSICmd(buffer, count) */
        uint16_t count = pop16();
        uint32_t cdb = pop32();
        int16_t err = 0;
        for (unsigned i = 0; i < count; i++) {
            if (mc_scsi_bus_phase(&bus) != MC_SCSI_COMMAND) { err = 5; break; }
            mc_scsi_bus_out(&bus, (uint8_t)rd8(cdb + i));
        }
        result(err);
        break;
    }
    case 5: case 6: {   /* SCSIRead(tib), SCSIWrite(tib) */
        uint32_t tib = pop32();
        result(tib_transfer(tib, selector == 6));
        break;
    }
    case 4: {   /* SCSIComplete(VAR stat, VAR message, wait) */
        pop32();
        uint32_t msg = pop32(), stat = pop32();
        while (mc_scsi_bus_phase(&bus) == MC_SCSI_DATA_IN) mc_scsi_bus_in(&bus, 0);
        while (mc_scsi_bus_phase(&bus) == MC_SCSI_DATA_OUT) mc_scsi_bus_out(&bus, 0);
        int16_t err = 0;
        if (mc_scsi_bus_phase(&bus) == MC_SCSI_STATUS) wr16(stat, mc_scsi_bus_in(&bus, 0)); else err = 10;
        if (mc_scsi_bus_phase(&bus) == MC_SCSI_MSG_IN) wr16(msg, mc_scsi_bus_in(&bus, 0)); else err = 10;
        if (mc_scsi_bus_phase(&bus) != MC_SCSI_BUS_FREE) mc_scsi_bus_release(&bus);
        result(err);
        break;
    }
    default:
        unknown_traps++;
        break;
    }
}

/* The documented SCSIAction A0/D0 contract over the same real SCSI bus.
 * A fixture SIM needs 32 private bytes beyond SCSI_IO: the driver must
 * query its size and clear all of them. No original API calls are needed
 * while a driver loader has blocked them (IM: Devices 4-11). */
static void scsi_action(void) {
    const uint32_t pb = cpu.r[8];
    CHECK_EQ(cpu.r[0], 1);
    switch (rd8(pb + 8)) {
    case 128: { /* SCSIGetVirtualIDInfo */
        const unsigned id = rd16(pb + 36);
        virtual_queries++;
        CHECK_EQ(rd16(pb + 6), 40);
        wr32(pb + 12, id << 8);
        wr8(pb + 38, id < 8 && bus.dev[id] != NULL);
        cpu.r[0] = 0;
        break;
    }
    case 3: /* SCSIBusInquiry */
        CHECK_EQ(rd16(pb + 6), 172);
        wr16(pb + 44, 208);
        cpu.r[0] = 0;
        break;
    case 1: { /* SCSIExecIO, synchronous */
        const unsigned id = rd8(pb + 14), cdb_len = rd8(pb + 53);
        const uint32_t cdb = rd32(pb + 68), data = rd32(pb + 40), bytes = rd32(pb + 44);
        CHECK_EQ(rd16(pb + 6), 208);
        CHECK_EQ(rd32(pb + 16), 0); /* no completion procedure */
        CHECK_EQ(rd32(pb + 20) & 0x21040000u, 0x21040000u);
        for (unsigned n = 176; n < 208; n++) CHECK_EQ(rd8(pb + n), 0);
        scsi43_execs++;
        if (id < 8) scsi43_pb[id] = pb;
        if (!mc_scsi_bus_arbitrate(&bus, 7) || !mc_scsi_bus_select(&bus, id, 0)) { cpu.r[0] = (uint32_t)-7934; break; }
        for (unsigned n = 0; n < cdb_len; n++) mc_scsi_bus_out(&bus, (uint8_t)rd8(cdb + n));
        uint32_t done = 0;
        while (done < bytes) {
            const int phase = mc_scsi_bus_phase(&bus);
            if (phase == MC_SCSI_DATA_IN) wr8(data + done, mc_scsi_bus_in(&bus, 0));
            else if (phase == MC_SCSI_DATA_OUT) mc_scsi_bus_out(&bus, (uint8_t)rd8(data + done));
            else break;
            done++;
        }
        CHECK_EQ(mc_scsi_bus_phase(&bus), MC_SCSI_STATUS);
        const unsigned status = mc_scsi_bus_in(&bus, 0);
        CHECK_EQ(mc_scsi_bus_phase(&bus), MC_SCSI_MSG_IN);
        mc_scsi_bus_in(&bus, 0);
        CHECK_EQ(mc_scsi_bus_phase(&bus), MC_SCSI_BUS_FREE);
        wr8(pb + 60, status);
        wr32(pb + 64, bytes - done);
        cpu.r[0] = status ? (uint32_t)-7932 : 0;
        break;
    }
    case 0x85: /* SCSICreateRefNumXref */
        xref_calls++;
        xref_function = rd8(pb + 8);
        xref_device = rd32(pb + 12);
        xref_refnum = (int16_t)rd16(pb + 36);
        cpu.r[0] = 0;
        break;
    default: CHECK(0); cpu.r[0] = (uint32_t)-50; break;
    }
    wr16(pb + 10, cpu.r[0]);
}

/* --- _ATAManager ------------------------------------------------------- */

#define ATA_ERR_CODE (-9399)        /* AT_UncDataErr, any device error */

static uint8_t ar(unsigned reg) { return (uint8_t)mc_ata_reg_read(&ata, reg, 1); }
static void aw(unsigned reg, uint8_t v) { mc_ata_reg_write(&ata, reg, 1, v); }
static uint8_t ata_settle(void) {
    for (int i = 0; i < 100000 && (ar(MC_ATA_ALT_STATUS) & MC_ATA_ST_BSY); i++) hw_fake_step(&fk, 1000);
    return ar(MC_ATA_ALT_STATUS);
}
static void ata_move_in(uint32_t at, uint32_t bytes) {
    for (uint32_t i = 0; i < bytes; i += 2) {
        uint16_t w = mc_ata_data_read16(&ata);
        wr8(at + i, w);
        if (i + 1 < bytes) wr8(at + i + 1, w >> 8);
    }
}
static void ata_move_out(uint32_t at, uint32_t bytes) {
    for (uint32_t i = 0; i < bytes; i += 2) mc_ata_data_write16(&ata, (uint16_t)(rd8(at + i) | rd8(at + i + 1) << 8));
}

/* ATA_ExecIO (Guide, "ATA_ExecIO" and Table 3-1). */
static int16_t ata_exec_io(uint32_t pb) {
    ata_execs++;
    uint16_t flags = (uint16_t)rd16(pb + 20);
    uint32_t buffer = rd32(pb + 56), count = rd32(pb + 60), packet = rd32(pb + 80), blind = rd32(pb + 52);
    unsigned protocol = (flags >> 4) & 3, dir = (flags >> 12) & 3;
    uint8_t cmd = (uint8_t)rd8(pb + 79);
    ata_cmds[cmd]++;
    if (ata_fail > 0) {
        ata_fail--;
        wr8(pb + 48, 0x51);
        wr8(pb + 49, 0x04);
        return ATA_ERR_CODE;
    }
    aw(MC_ATA_FEATURES, (uint8_t)rd8(pb + 72));
    aw(MC_ATA_COUNT, (uint8_t)rd8(pb + 73));
    aw(MC_ATA_SECTOR, (uint8_t)rd8(pb + 74));
    aw(MC_ATA_CYL_HI, (uint8_t)rd8(pb + 76));
    aw(MC_ATA_CYL_LO, (uint8_t)rd8(pb + 77));
    aw(MC_ATA_DRIVE, (uint8_t)rd8(pb + 78));
    aw(MC_ATA_COMMAND, cmd);
    uint8_t st = 0;
    uint32_t done = 0;
    int bad = 0;
    if (protocol == 2 && packet) {        /* ATAPI: ProtocolType $20, as the G3 ROM's own packets ($2884CA) */
        st = ata_settle();
        if ((st & 0xC9) != 0x48) { bad = 1; }
        else for (unsigned i = 0; i < 6; i++) mc_ata_data_write16(&ata, (uint16_t)(rd8(packet + 2 + 2 * i) | rd8(packet + 3 + 2 * i) << 8));
        for (;;) {
            st = ata_settle();
            if (bad || !(st & MC_ATA_ST_DRQ)) break;
            uint32_t chunk = ar(MC_ATA_CYL_LO) | ar(MC_ATA_CYL_HI) << 8;
            if (ar(MC_ATA_COUNT) & MC_ATAPI_IO) ata_move_in(buffer + done, chunk); else ata_move_out(buffer + done, chunk);
            done += chunk;
        }
    } else if (dir == 2 || dir == 1) {
        while (done < count) {
            st = ata_settle();
            if ((st & (MC_ATA_ST_ERR | MC_ATA_ST_DRQ)) != MC_ATA_ST_DRQ) { bad = 1; break; }
            uint32_t n = count - done < blind ? count - done : blind;
            if (dir == 2) ata_move_in(buffer + done, n); else ata_move_out(buffer + done, n);
            done += n;
        }
        if (!bad) st = ata_settle();
    } else {
        st = ata_settle();
    }
    ar(MC_ATA_STATUS);
    wr8(pb + 48, st);
    wr8(pb + 49, ar(MC_ATA_ERROR));
    wr32(pb + 64, done);
    return (bad || (st & MC_ATA_ST_ERR)) ? ATA_ERR_CODE : 0;
}

static void ata_manager(void) {
    uint32_t pb = pop32();
    if (rd8(pb + 6) != 1) { result(-50); ata_unknown++; return; }
    switch (rd8(pb + 18)) {
    case 0x01: result(ata_exec_io(pb)); break;
    case 0x85:      /* ATA_DrvrRegister */
        ata_registers++;
        ata_reg_refnum = (int16_t)rd16(pb + 48);
        ata_reg_device = rd32(pb + 24);
        result(0);
        break;
    default: ata_unknown++; result(-9358); break;
    }
}

static void line_a(uint16_t op) {
    switch (op) {
    case 0xA03D: {      /* _DrvrInstall: A0 driver, D0 refNum */
        int16_t ref = (int16_t)cpu.r[0];
        uint32_t handle = heap, dce = heap + 4;
        heap += 4 + 64;
        wr32(handle, dce);
        memset(ram + dce, 0, 64);
        wr16(dce + 24, (uint16_t)ref);      /* dCtlRefNum */
        wr16(dce + 4, 0x0040);              /* dRAMBased (Devices 1-83) */
        wr32(UNIT_TABLE + 4u * (uint16_t)~ref, handle);
        cpu.r[0] = 0;
        break;
    }
    case 0xA029: cpu.r[0] = 0; break;       /* _HLock */
    case 0xA04E: {      /* _AddDrive: A0 element, D0 drive number:refNum */
        uint32_t el = cpu.r[8];
        wr16(el + 6, cpu.r[0] >> 16);
        wr16(el + 8, cpu.r[0] & 0xFFFF);
        wr32(el, 0);
        if (!rd32(0x30A)) wr32(0x30A, el); else wr32(rd32(0x30E), el);
        wr32(0x30E, el);
        cpu.r[0] = 0;
        break;
    }
    case 0xA02F:        /* _PostEvent: A0 event code, D0 message */
        if (cpu.r[8] == 7 && events < 8) event_msg[events++] = cpu.r[0];
        cpu.r[0] = 0;
        break;
    case 0xA71E:        /* _NewPtrSysClear */
    case 0xA51E:        /* _NewPtrSys: D0 size, A0 the block */
        if (op == 0xA71E) memset(ram + heap, 0, cpu.r[0]);
        cpu.r[8] = heap;
        heap += (cpu.r[0] + 3u) & ~3u;
        cpu.r[0] = 0;
        break;
    case 0xA02E:        /* _BlockMove: A0 source, A1 destination, D0 count */
        memmove(ram + cpu.r[9], ram + cpu.r[8], cpu.r[0]);
        cpu.r[0] = 0;
        break;
    case 0xA815: scsi_dispatch(); break;
    case 0xAAF1: ata_manager(); break;
    case 0xA346: /* GetOSTrapAddress(_Gestalt) */
        CHECK_EQ(cpu.r[0] & 0xFFFF, 0xA1AD);
        cpu.r[8] = scsi43_available ? 0xF020 : 0xF010;
        break;
    case 0xA746: /* GetToolboxTrapAddress(_Unimplemented) */
        CHECK_EQ(cpu.r[0] & 0xFFFF, 0xA89F);
        cpu.r[8] = 0xF010;
        break;
    case 0xA1AD: /* Gestalt('scsi') */
        gestalt_calls++;
        if (cpu.r[0] == 0x73637369u && scsi43_available) { cpu.r[0] = 0; cpu.r[8] = 1; }
        else { cpu.r[0] = (uint32_t)-5551; cpu.r[8] = 0; }
        break;
    case 0xA089: scsi_action(); break;
    default:
        fprintf(stderr, "trap %04X\n", op);
        unknown_traps++;
        break;
    }
}

/* Calls `entry` as a subroutine and runs until it returns. */
static int call(uint32_t entry) {
    cpu.r[15] -= 4;
    wr32(cpu.r[15], SENTINEL);
    cpu.pc = entry;
    for (long i = 0; i < 20000000; i++) {
        if (cpu.pc == SENTINEL) return 1;
        if (cpu.pc == IODONE) iodones++;
        uint16_t op = (uint16_t)rd16(cpu.pc);
        if ((op & 0xF000) == 0xA000) {
            line_a(op);
            cpu.pc += 2;
            continue;
        }
        m68k_run(&cpu, 1);
        if (exceptions || cpu.status != M68K_OK) return 0;
    }
    return 0;
}

static void setup_cpu(void) {
    memset(ram, 0, sizeof ram);
    wr32(0, 0x1F0000);      /* reset: SSP and PC */
    wr32(4, SENTINEL);
    wr32(0x11C, UNIT_TABLE);
    wr16(0x1D2, 64);        /* UnitNtryCnt */
    wr16(0x308, 0);         /* DrvQHdr: qFlags, qHead, qTail */
    wr32(0x30A, 0);
    wr32(0x30E, 0);
    wr32(0x8FC, IODONE);
    wr16(IODONE, 0x4E75);   /* RTS */
    wr16(SENTINEL, 0x4E71);
    m68k_bus b = {0};
    b.read = bus_read;
    b.write = bus_write;
    m68k_init(&cpu, M68K_68040, &b);
    cpu.on_exception = on_exception;
    m68k_reset(&cpu);
}

static void setup(void) {
    char error[200];
    img1 = mc_disk_open(scsi_make_image("livadixdisk", "hfs1.img", 300 * 512, 0x71), 1, 512, error, sizeof error);
    img2 = mc_disk_open(scsi_make_image("livadixdisk", "hfs2.img", 100 * 512, 0x72), 0, 512, error, sizeof error);
    iso = mc_disk_open(scsi_make_image("livadixdisk", "cd.iso", 50 * 2048, 0x73), 0, 2048, error, sizeof error);
    CHECK(img1 && img2 && iso);
    mc_vdisk_drivers d = {0};
    d.scsi = mc_guest_livadixdisk(&d.scsi_size);
    mc_disk *both[2] = {img1, img2};
    vd = mc_vdisk_create(both, 2, &d, error, sizeof error);
    vcd = mc_vdisk_create(&iso, 1, &d, error, sizeof error);
    CHECK(vd && vcd);
    mc_scsi_bus_init(&bus);
    mc_scsi_disk_init(&disk, vd);
    mc_scsi_cd_init(&cd);
    mc_scsi_cd_insert(&cd, vcd);
    mc_scsi_bus_attach(&bus, 2, mc_scsi_unit_target(&disk));
    mc_scsi_bus_attach(&bus, 3, mc_scsi_unit_target(&cd));
    setup_cpu();
}

/* What the Start Manager does: read block 0, load ddSize blocks from
 * ddBlock of the descriptor of type 1. */
static void load_descriptor(mc_vdisk *v, uint32_t at, unsigned index, unsigned type) {
    uint8_t b0[512];
    CHECK(mc_vdisk_read(v, 0, 1, b0));
    const uint8_t *d = b0 + 18 + 8 * index;
    uint32_t block = (uint32_t)d[0] << 24 | (uint32_t)d[1] << 16 | (uint32_t)d[2] << 8 | d[3];
    uint32_t size = (uint32_t)d[4] << 8 | d[5];
    CHECK_EQ(d[6] << 8 | d[7], type);
    CHECK(mc_vdisk_read(v, block, size, ram + at));
}
static void load_driver(mc_vdisk *v, uint32_t at) { load_descriptor(v, at, 0, 1); }

static uint32_t dce_of(int16_t ref) { return rd32(rd32(UNIT_TABLE + 4u * (uint16_t)~ref)); }
static uint32_t drive_el(unsigned drive) {
    for (uint32_t el = rd32(0x30A); el; el = rd32(el))
        if (rd16(el + 6) == drive) return el;
    return 0;
}

/* A Device Manager call of the routine at header offset `which` (8 Open,
 * 10 Prime, 12 Control, 14 Status). Returns D0. */
static int16_t routine(uint32_t driver, unsigned which, int16_t ref) {
    uint32_t header = driver + 16;
    cpu.r[8] = PB;
    cpu.r[9] = dce_of(ref);
    CHECK(call(header + rd16(header + which)));
    return (int16_t)cpu.r[0];
}

/* The periodic call (Control, csCode accRun 65). */
static int16_t acc_run(uint32_t driver, int16_t ref) {
    memset(ram + PB, 0, 80);
    wr16(PB + 26, 65);
    return routine(driver, 12, ref);
}

static void pb_rw(int write, unsigned drive, uint32_t position, uint32_t count, int16_t ref) {
    memset(ram + PB, 0, 80);
    wr16(PB + 6, write ? 0xA003 : 0xA002);
    wr16(PB + 22, drive);
    wr16(PB + 24, (uint16_t)ref);
    wr32(PB + 32, BUFFER);
    wr32(PB + 36, count);
    wr32(dce_of(ref) + 16, position);
}

/* Installation through the +0 entry with D5 = SCSI ID 2. */
static void test_install(void) {
    load_driver(vd, DRIVER);
    CHECK_EQ(rd16(DRIVER), 0x6000);     /* BRA.W at +0 and +8 */
    CHECK_EQ(rd16(DRIVER + 8), 0x6000);
    cpu.r[5] = 2;
    CHECK(call(DRIVER));
    CHECK_EQ(cpu.r[0] & 0xFFFF, 0);
    CHECK_EQ(cpu.r[5], 2);              /* registers preserved */
    CHECK_EQ(gestalt_calls, 0);          /* no Gestalt trap on an old ROM */
    CHECK_EQ(unknown_traps, 0);
    CHECK_EQ(exceptions, 0);
    uint32_t dce = dce_of(-35);
    CHECK_EQ(rd32(dce), DRIVER + 16);   /* dCtlDriver: the header */
    CHECK_EQ(rd16(DRIVER + 16), 0x6F00);
    CHECK_EQ(rd16(dce + 4), 0x6F20);    /* driver flags, dOpened, not dRAMBased */
    CHECK_EQ((int16_t)rd16(dce + 24), -35);
    /* Two Apple_HFS partitions: drives 8 and 9 with the refNum, sizes and
     * flags; a disk-inserted event for each at the first periodic call,
     * after which a disk's driver needs no more. */
    uint32_t a = drive_el(8), b = drive_el(9);
    CHECK(a && b);
    CHECK_EQ((int16_t)rd16(a + 8), -35);
    CHECK_EQ(rd16(a + 4), 1);           /* qType */
    CHECK_EQ(rd16(a + 12) | rd16(a + 14) << 16, 300);
    CHECK_EQ(rd16(b + 12) | rd16(b + 14) << 16, 100);
    CHECK_EQ(rd8(a - 4), 0x00); CHECK_EQ(rd8(a - 3), 8);
    CHECK_EQ(rd8(b - 4), 0x80);         /* the read-only image: locked */
    CHECK_EQ(events, 0);
    CHECK_EQ(acc_run(DRIVER, -35), 0);
    CHECK_EQ(events, 2);
    CHECK_EQ(event_msg[0], 8); CHECK_EQ(event_msg[1], 9);
    CHECK_EQ(rd16(dce + 4), 0x4F20);    /* dNeedTime off */
    /* The CD-ROM drive at ID 3 has a disc: its own driver loads from it. */
    CHECK_EQ(rd32(UNIT_TABLE + 4u * 35), 0);
}

/* Prime: partition-relative blocks, dCtlPosition advanced, ioActCount set,
 * IODone called; transfers over 128 blocks in pieces; writes to the image;
 * the locked drive refuses writes (wPrErr); range (ioErr), alignment
 * (paramErr) and unknown drives (nsDrvErr). */
static void prime_suite(uint32_t drv, int16_t REF, mc_disk *image1, mc_disk *image2);
static void test_prime(void) { prime_suite(DRIVER, -35, img1, img2); }
static void prime_suite(uint32_t drv, int16_t REF, mc_disk *image1, mc_disk *image2) {
    uint8_t want[512 * 4];
    pb_rw(0, 8, 3 * 512, 1024, REF);
    int before = iodones;
    CHECK_EQ(routine(drv, 10, REF), 0);
    CHECK_EQ(iodones, before + 1);
    CHECK(mc_disk_read(image1, 3, 2, want));
    CHECK(!memcmp(ram + BUFFER, want, 1024));
    CHECK_EQ(rd32(PB + 40), 1024);
    CHECK_EQ(rd32(dce_of(REF) + 16), 5 * 512);
    pb_rw(0, 9, 7 * 512, 512, REF);
    CHECK_EQ(routine(drv, 10, REF), 0);
    CHECK(mc_disk_read(image2, 7, 1, want) && !memcmp(ram + BUFFER, want, 512));
    pb_rw(0, 8, 0, 300 * 512, REF);
    CHECK_EQ(routine(drv, 10, REF), 0);
    static uint8_t all[300 * 512];
    CHECK(mc_disk_read(image1, 0, 300, all));
    CHECK(!memcmp(ram + BUFFER, all, sizeof all));
    for (int i = 0; i < 1024; i++) ram[BUFFER + i] = (uint8_t)(i * 13);
    pb_rw(1, 8, 20 * 512, 1024, REF);
    CHECK_EQ(routine(drv, 10, REF), 0);
    CHECK(mc_disk_read(image1, 20, 2, want) && !memcmp(want, ram + BUFFER, 1024));
    pb_rw(1, 9, 0, 512, REF);
    CHECK_EQ(routine(drv, 10, REF), -44);
    pb_rw(0, 8, 299 * 512, 1024, REF);
    CHECK_EQ(routine(drv, 10, REF), -36);
    pb_rw(0, 8, 100, 512, REF);
    CHECK_EQ(routine(drv, 10, REF), -50);
    pb_rw(0, 3, 0, 512, REF);
    CHECK_EQ(routine(drv, 10, REF), -56);
    /* Wide positioning: ioPosMode bit 8, 64-bit offset at 46. */
    pb_rw(0, 8, 0, 512, REF);
    wr16(PB + 44, 0x0100);
    wr32(PB + 46, 0);
    wr32(PB + 50, 6 * 512);
    CHECK_EQ(routine(drv, 10, REF), 0);
    CHECK(mc_disk_read(image1, 6, 1, want) && !memcmp(ram + BUFFER, want, 512));
    /* An immediate call (noQueueBit) returns without IODone. */
    pb_rw(0, 8, 0, 512, REF);
    wr16(PB + 6, 0xA202);
    before = iodones;
    CHECK_EQ(routine(drv, 10, REF), 0);
    CHECK_EQ(iodones, before);
}

/* Control 21/22 (icons), 23 (drive information), 5/6 accepted, unknown
 * codes controlErr; Status 8 (DrvSts), unknown statusErr; Open noErr,
 * Close closErr. */
static void test_control_status(void) {
    memset(ram + PB, 0, 80);
    wr16(PB + 22, 8);
    wr16(PB + 26, 21);
    CHECK_EQ(routine(DRIVER, 12, -35), 0);
    uint32_t icon = rd32(PB + 28);
    CHECK(icon > DRIVER && icon < DRIVER + 4096);
    CHECK_EQ(rd32(icon + 36), 0x7FFFFFFE);
    CHECK_EQ(rd32(icon + 128 + 36), 0x7FFFFFFE);
    CHECK_EQ(rd8(icon + 256), 0);
    wr16(PB + 26, 22);
    CHECK_EQ(routine(DRIVER, 12, -35), 0);
    CHECK_EQ(rd32(PB + 28), icon);
    wr16(PB + 26, 23);
    CHECK_EQ(routine(DRIVER, 12, -35), 0);
    CHECK_EQ(rd32(PB + 28), 0x0601);
    wr16(PB + 26, 5);
    CHECK_EQ(routine(DRIVER, 12, -35), 0);
    wr16(PB + 26, 99);
    CHECK_EQ(routine(DRIVER, 12, -35), -17);
    wr16(PB + 26, 8);
    wr16(PB + 22, 9);
    CHECK_EQ(routine(DRIVER, 14, -35), 0);
    CHECK_EQ(rd8(PB + 30), 0x80);       /* writeProt */
    CHECK_EQ(rd8(PB + 31), 8);          /* diskInPlace */
    CHECK_EQ(rd8(PB + 32), 1);          /* installed */
    CHECK_EQ(rd16(PB + 40), 9);         /* dQDrive */
    CHECK_EQ((int16_t)rd16(PB + 42), -35);
    wr16(PB + 26, 77);
    CHECK_EQ(routine(DRIVER, 14, -35), -18);
    CHECK_EQ(routine(DRIVER, 8, -35), 0);
    CHECK_EQ(routine(DRIVER, 16, -35), -24);
}

/* An old-entry driver survives the loader's legacy-call guard by resolving
 * its virtual ID and using the 4.3 transaction API. Read real image bytes;
 * the test's SCSIGet would return scSequenceErr if it were attempted. */
static void test_scsi43(void) {
    scsi43_available = legacy_blocked = 1;
    const unsigned before = legacy_calls;
    pb_rw(0, 8, 7 * 512, 512, -35);
    CHECK_EQ(routine(DRIVER, 10, -35), 0);
    uint8_t want[512];
    CHECK(mc_disk_read(img1, 7, 1, want) && !memcmp(ram + BUFFER, want, 512));
    CHECK(scsi43_execs > 0 && virtual_queries == 1);
    CHECK_EQ(legacy_calls, before);
}

/* The CD-ROM through the SCSI Manager 4.3 entry (+8, D5 = DeviceIdent with
 * target 3): the unit attention of the inserted disc is retried, the drive
 * is set to 512-byte blocks (switched there from 2048 if it was), the drive
 * is locked and ejectable, the
 * driver registers with the XPT (SCSICreateRefNumXref $85), reads work and
 * Control 7 ejects the disc. */
static int ejected;
static void on_eject(void *ctx, mc_scsi_unit *u, mc_vdisk *media) { (void)ctx; (void)u; (void)media; ejected++; }

static void test_cd(void) {
    load_driver(vcd, DRIVER_CD);
    cd.block = 2048;                  /* a drive another driver left in 2048-byte mode */
    cpu.r[5] = 0x00000300;
    CHECK(call(DRIVER_CD + 8));
    CHECK_EQ(cpu.r[0] & 0xFFFF, 0);
    CHECK_EQ(cd.block, 512);
    CHECK_EQ(xref_calls, 1);
    CHECK_EQ(xref_function, 0x85);
    CHECK_EQ(xref_device, 0x300);
    CHECK_EQ(xref_refnum, -36);
    uint32_t el = drive_el(10);
    CHECK(el != 0);
    CHECK_EQ((int16_t)rd16(el + 8), -36);
    CHECK_EQ(rd16(el + 12), 200);
    CHECK_EQ(rd8(el - 4), 0x80);
    CHECK_EQ(rd8(el - 3), 1);
    CHECK_EQ(events, 2);
    const uint32_t dce = dce_of(-36);
    CHECK_EQ(rd16(dce + 34), 60);       /* dCtlDelay: looks for a disc once a second */
    CHECK_EQ(acc_run(DRIVER_CD, -36), 0);
    CHECK_EQ(events, 3);
    CHECK_EQ(event_msg[2], 10);
    CHECK_EQ(rd16(dce + 4), 0x6F20);    /* dNeedTime kept */
    CHECK_EQ(acc_run(DRIVER_CD, -36), 0);
    CHECK_EQ(events, 3);
    pb_rw(0, 10, 4 * 512, 512, -36);
    CHECK_EQ(routine(DRIVER_CD, 10, -36), 0);
    uint8_t want[2048];
    CHECK(mc_disk_read(iso, 1, 1, want) && !memcmp(ram + BUFFER, want, 512));
    pb_rw(1, 10, 0, 512, -36);
    CHECK_EQ(routine(DRIVER_CD, 10, -36), -44);
    memset(ram + PB, 0, 80);
    wr16(PB + 22, 10);
    wr16(PB + 26, 23);
    CHECK_EQ(routine(DRIVER_CD, 12, -36), 0);
    CHECK_EQ(rd32(PB + 28), 0x0201);
    mc_scsi_unit_on_eject(&cd, on_eject, NULL);
    wr16(PB + 26, 7);
    CHECK_EQ(routine(DRIVER_CD, 12, -36), 0);
    CHECK_EQ(ejected, 1);
    CHECK(cd.media == NULL);
    CHECK_EQ(rd8(el - 3), 0);
    CHECK_EQ(acc_run(DRIVER_CD, -36), 0);
    CHECK_EQ(events, 3);
    /* The disc goes back in (Ctrl+Alt+R): its unit attention, the same
     * drive with the disc in place, a disk-inserted event. */
    mc_scsi_cd_insert(&cd, vcd);
    CHECK_EQ(acc_run(DRIVER_CD, -36), 0);
    CHECK_EQ(events, 4);
    CHECK_EQ(event_msg[3], 10);
    CHECK_EQ(drive_el(10), el);
    CHECK(drive_el(11) == 0);
    CHECK_EQ(rd8(el - 3), 1);
    pb_rw(0, 10, 4 * 512, 512, -36);
    CHECK_EQ(routine(DRIVER_CD, 10, -36), 0);
    CHECK(!memcmp(ram + BUFFER, want, 512));
    wr16(PB + 22, 10);
    wr16(PB + 26, 7);
    CHECK_EQ(routine(DRIVER_CD, 12, -36), 0);
    /* The disk driver ignores eject. */
    wr16(PB + 22, 8);
    CHECK_EQ(routine(DRIVER, 12, -35), 0);
    CHECK(disk.media == vd);
    CHECK_EQ(exceptions, 0);
    CHECK_EQ(unknown_traps, 0);
}

/* An empty CD-ROM drive at startup: the disk's driver (SCSI Manager 4.3
 * entry) installs a copy of itself for ID 3, registered with the XPT, with
 * no drive; a disc inserted later is found by the periodic call, gets a
 * drive and a disk-inserted event, and reads. */
static void test_adopt(void) {
    setup_cpu();
    events = 0;
    CHECK(cd.media == NULL);
    load_driver(vd, DRIVER);
    const unsigned xrefs = xref_calls;
    cpu.r[5] = 0x00000200;
    CHECK(call(DRIVER + 8));
    CHECK_EQ(cpu.r[0] & 0xFFFF, 0);
    CHECK_EQ(xref_calls, xrefs + 2);     /* the copy's ($300), then the disk's */
    CHECK_EQ(xref_device, 0x200);
    CHECK_EQ(xref_refnum, -35);
    CHECK(rd32(UNIT_TABLE + 4u * 35) != 0);
    const uint32_t dce = dce_of(-36), copy = rd32(dce) - 16;
    CHECK(copy >= HEAP && copy < DRIVER);
    CHECK_EQ(rd16(dce + 34), 60);
    CHECK(drive_el(10) == 0);
    CHECK_EQ(acc_run(DRIVER, -35), 0);
    CHECK_EQ(events, 2);
    CHECK_EQ(acc_run(copy, -36), 0);
    CHECK_EQ(events, 2);
    mc_scsi_cd_insert(&cd, vcd);
    CHECK_EQ(acc_run(copy, -36), 0);
    CHECK_EQ(events, 3);
    CHECK_EQ(event_msg[2], 10);
    const uint32_t el = drive_el(10);
    CHECK(el != 0);
    CHECK_EQ((int16_t)rd16(el + 8), -36);
    CHECK_EQ(rd16(el + 12), 200);
    CHECK_EQ(rd8(el - 3), 1);
    pb_rw(0, 10, 4 * 512, 512, -36);
    CHECK_EQ(routine(copy, 10, -36), 0);
    uint8_t want[2048];                 /* a 2048-byte sector of the image */
    CHECK(mc_disk_read(iso, 1, 1, want) && !memcmp(ram + BUFFER, want, 512));
    CHECK_EQ(exceptions, 0);
    CHECK_EQ(unknown_traps, 0);
    CHECK(scsi43_pb[2] && scsi43_pb[3] && scsi43_pb[2] != scsi43_pb[3]);
}


/* --- the ATA flavour ------------------------------------------------------ */

#define DRIVER_ATA 0x10000u
#define DRIVER_ATA_CD 0x20000u
static mc_disk *aimg1, *aimg2, *aiso;
static mc_vdisk *avd, *avcd;
static int ata_ejected;

static void on_ata_eject(void *ctx, unsigned dev, mc_vdisk *media) { (void)ctx; (void)media; CHECK_EQ(dev, 1); ata_ejected++; }

static void setup_ata(void) {
    char error[200];
    aimg1 = mc_disk_open(scsi_make_image("livadixdisk", "ata1.img", 300 * 512, 0x81), 1, 512, error, sizeof error);
    aimg2 = mc_disk_open(scsi_make_image("livadixdisk", "ata2.img", 100 * 512, 0x82), 0, 512, error, sizeof error);
    aiso = mc_disk_open(scsi_make_image("livadixdisk", "ata.iso", 50 * 2048, 0x83), 0, 2048, error, sizeof error);
    CHECK(aimg1 && aimg2 && aiso);
    mc_vdisk_drivers d = {0};
    d.scsi = mc_guest_livadixdisk(&d.scsi_size);
    d.ata = mc_guest_livadixdisk_ata(&d.ata_size);
    mc_disk *both[2] = {aimg1, aimg2};
    avd = mc_vdisk_create(both, 2, &d, error, sizeof error);
    avcd = mc_vdisk_create(&aiso, 1, &d, error, sizeof error);
    CHECK(avd && avcd);
    hw_fake_init(&fk);
    mc_ata_init(&ata, &fk.host, 2);
    mc_ata_attach_disk(&ata, 0, avd);
    mc_ata_attach_cd(&ata, 1);
    mc_ata_on_eject(&ata, on_ata_eject, NULL);
    mc_ata_cd_insert(&ata, 1, avcd);
    mc_ata_reset(&ata);
    hw_fake_step(&fk, MC_ATA_T_RESET + 1);
    setup_cpu();
    events = 0;
}

/* The ATA partition holds the ATA flavour: descriptor 2 of block 0 has type
 * $0701; entered at +0 with the ataPBDeviceID in D5 (devNum 0), it identifies
 * the device (IDENTIFY DEVICE), registers with the ATA Manager under the
 * first free reference number from -54, installs in that unit and adds a
 * drive per Apple_HFS partition. */
static void test_ata_install(void) {
    load_descriptor(avd, DRIVER_ATA, 1, 0x0701);
    CHECK_EQ(rd16(DRIVER_ATA), 0x6000);
    CHECK_EQ(rd16(DRIVER_ATA + 8), 0x6000);
    cpu.r[5] = 0x00000000;
    cpu.r[3] = 0x1234;
    CHECK(call(DRIVER_ATA));
    CHECK_EQ(cpu.r[0] & 0xFFFF, 0);
    CHECK_EQ(cpu.r[5], 0);
    CHECK_EQ(cpu.r[3], 0x1234);         /* registers preserved */
    CHECK_EQ(unknown_traps, 0);
    CHECK_EQ(ata_unknown, 0);
    CHECK_EQ(exceptions, 0);
    CHECK_EQ(ata_registers, 1);
    CHECK_EQ(ata_reg_refnum, -54);
    CHECK_EQ(ata_reg_device, 0);
    CHECK_EQ(ata_cmds[0xEC], 1);        /* IDENTIFY DEVICE answered: a disk */
    CHECK_EQ(ata_cmds[0xA1], 0);
    uint32_t dce = dce_of(-54);
    CHECK_EQ(rd32(dce), DRIVER_ATA + 16);
    CHECK_EQ(rd16(DRIVER_ATA + 16), 0x6F00);
    CHECK_EQ(rd16(dce + 4), 0x6F24);
    CHECK_EQ((int16_t)rd16(dce + 24), -54);
    CHECK_EQ(rd8(DRIVER_ATA + 16 + 18), 11);                 /* the name: ".LivadixATA" */
    CHECK(!memcmp(ram + DRIVER_ATA + 16 + 19, ".LivadixATA", 11));
    uint32_t a = drive_el(8), b = drive_el(9);
    CHECK(a && b);
    CHECK_EQ((int16_t)rd16(a + 8), -54);
    CHECK_EQ(rd16(a + 12) | rd16(a + 14) << 16, 300);
    CHECK_EQ(rd16(b + 12) | rd16(b + 14) << 16, 100);
    CHECK_EQ(rd8(a - 4), 0x00); CHECK_EQ(rd8(a - 3), 8);
    CHECK_EQ(rd8(b - 4), 0x80);
    CHECK_EQ(acc_run(DRIVER_ATA, -54), 0);
    CHECK_EQ(events, 2);
    CHECK_EQ(event_msg[0], 8); CHECK_EQ(event_msg[1], 9);
    CHECK_EQ(rd16(dce + 4), 0x4F24);    /* dNeedTime off, DriverGestalt retained */
    /* The partition map was read through READ SECTORS (20h): LBA addressing, the DEV bit clear. */
    CHECK(ata_cmds[0x20] > 0);
}

/* A transient error from the ATA Manager is retried (three tries); a
 * persistent one is an ioErr; writes are WRITE SECTORS (30h). */
static void test_ata_retry(void) {
    uint8_t want[512];
    ata_fail = 2;
    pb_rw(0, 8, 5 * 512, 512, -54);
    CHECK_EQ(routine(DRIVER_ATA, 10, -54), 0);
    CHECK(mc_disk_read(aimg1, 5, 1, want) && !memcmp(ram + BUFFER, want, 512));
    ata_fail = 3;
    pb_rw(0, 8, 5 * 512, 512, -54);
    CHECK_EQ(routine(DRIVER_ATA, 10, -54), -36);
    ata_fail = 0;
    unsigned writes = ata_cmds[0x30];
    for (int i = 0; i < 512; i++) ram[BUFFER + i] = (uint8_t)(i * 5 + 1);
    pb_rw(1, 8, 40 * 512, 512, -54);
    CHECK_EQ(routine(DRIVER_ATA, 10, -54), 0);
    CHECK_EQ(ata_cmds[0x30], writes + 1);
    CHECK(mc_disk_read(aimg1, 40, 1, want) && !memcmp(ram + BUFFER, want, 512));
}

/* Control and status as the ATA disk driver defines them (Guide, chapter 2):
 * 5 verify, 6 format (no-ops), 7 eject (nothing for a hard disk), 21/22 icons,
 * 23 drive characteristics $0601, unknown controlErr; status 8 drive
 * status, 43 driver gestalt ('sync', 'devt', 'intf', 'wide', ...), unknown
 * statusErr; close is refused as the SCSI flavour's. */
static void test_ata_control_status(void) {
    memset(ram + PB, 0, 80);
    wr16(PB + 22, 8);
    wr16(PB + 26, 21);
    CHECK_EQ(routine(DRIVER_ATA, 12, -54), 0);
    uint32_t icon = rd32(PB + 28);
    CHECK(icon > DRIVER_ATA && icon < DRIVER_ATA + 8192);
    CHECK_EQ(rd32(icon + 36), 0x7FFFFFFE);
    wr16(PB + 26, 22);
    CHECK_EQ(routine(DRIVER_ATA, 12, -54), 0);
    CHECK_EQ(rd32(PB + 28), icon);
    wr16(PB + 26, 23);
    CHECK_EQ(routine(DRIVER_ATA, 12, -54), 0);
    CHECK_EQ(rd32(PB + 28), 0x0601);
    wr16(PB + 26, 5);
    CHECK_EQ(routine(DRIVER_ATA, 12, -54), 0);
    wr16(PB + 26, 6);
    CHECK_EQ(routine(DRIVER_ATA, 12, -54), 0);
    wr16(PB + 26, 7);
    CHECK_EQ(routine(DRIVER_ATA, 12, -54), 0);
    CHECK(ata_cmds[0xA0] == 0);
    wr16(PB + 26, 99);
    CHECK_EQ(routine(DRIVER_ATA, 12, -54), -17);
    wr16(PB + 26, 8);
    wr16(PB + 22, 9);
    CHECK_EQ(routine(DRIVER_ATA, 14, -54), 0);
    CHECK_EQ(rd8(PB + 30), 0x80); CHECK_EQ(rd8(PB + 31), 8); CHECK_EQ(rd8(PB + 32), 1);
    CHECK_EQ(rd16(PB + 40), 9); CHECK_EQ((int16_t)rd16(PB + 42), -54);
    static const struct { uint32_t selector, answer; int ok; } g[] = {
        {0x73796E63, 1, 1},             /* 'sync' */
        {0x64657674, 0x6469736B, 1},    /* 'devt' = 'disk' */
        {0x696E7466, 0x69646520, 1},    /* 'intf' = 'ide ' */
        {0x77696465, 1, 1},             /* 'wide' */
        {0x70757267, 0, 1},             /* 'purg' */
        {0x7A7A7A7A, 0, 0},             /* unknown */
    };
    static const int16_t gestalt_vrefs[] = {8, 0, 3};
    for (unsigned v = 0; v < sizeof gestalt_vrefs / sizeof gestalt_vrefs[0]; v++) {
        for (unsigned i = 0; i < sizeof g / sizeof g[0]; i++) {
            memset(ram + PB, 0, 80);
            wr16(PB + 22, gestalt_vrefs[v]);
            wr16(PB + 26, 43);
            wr32(PB + 28, g[i].selector);
            wr32(PB + 32, 0xDEADBEEF);
            CHECK_EQ(routine(DRIVER_ATA, 14, -54), g[i].ok ? 0 : -18);
            CHECK_EQ(rd32(PB + 32), g[i].ok ? g[i].answer : 0xDEADBEEF);
        }
    }
    wr16(PB + 22, 3);
    wr16(PB + 26, 8);
    CHECK_EQ(routine(DRIVER_ATA, 14, -54), -56);
    wr16(PB + 22, 8);
    wr16(PB + 26, 77);
    CHECK_EQ(routine(DRIVER_ATA, 14, -54), -18);
    CHECK_EQ(routine(DRIVER_ATA, 8, -54), 0);
    CHECK_EQ(routine(DRIVER_ATA, 16, -54), -24);
}

/* An ATAPI CD-ROM drive as device 1 (ataPBDeviceID $0100): IDENTIFY DEVICE
 * is refused, IDENTIFY PACKET DEVICE answers; the next free reference number
 * (-55); TEST UNIT READY retried over the disc's unit attention; MODE SELECT(10)
 * to 512-byte blocks; the drive is locked and ejectable; polling finds a disc
 * taken out and put back; Control 7 ejects with START STOP UNIT. */
static void test_ata_cd(void) {
    load_descriptor(avcd, DRIVER_ATA_CD, 1, 0x0701);
    const unsigned execs = ata_execs, ident = ata_cmds[0xEC], idp = ata_cmds[0xA1];
    cpu.r[5] = 0x00000100;
    CHECK(call(DRIVER_ATA_CD));
    CHECK_EQ(cpu.r[0] & 0xFFFF, 0);
    CHECK_EQ(unknown_traps, 0);
    CHECK_EQ(ata_unknown, 0);
    CHECK_EQ(exceptions, 0);
    CHECK_EQ(ata_registers, 2);
    CHECK_EQ(ata_reg_refnum, -55);
    CHECK_EQ(ata_reg_device, 0x100);
    CHECK_EQ(ata_cmds[0xEC], ident + 1);
    CHECK_EQ(ata_cmds[0xA1], idp + 1);
    CHECK(ata_execs > execs + 5);
    CHECK_EQ(ata.unit[1].scsi.block, 512);      /* switched by MODE SELECT(10) */
    uint32_t el = drive_el(10);
    CHECK(el != 0);
    CHECK_EQ((int16_t)rd16(el + 8), -55);
    CHECK_EQ(rd16(el + 12), 200);
    CHECK_EQ(rd8(el - 4), 0x80);
    CHECK_EQ(rd8(el - 3), 1);
    const uint32_t dce = dce_of(-55);
    CHECK_EQ(rd16(dce + 34), 60);
    const unsigned e0 = events;
    CHECK_EQ(acc_run(DRIVER_ATA_CD, -55), 0);
    CHECK_EQ(events, e0 + 1);
    CHECK_EQ(event_msg[e0], 10);
    CHECK_EQ(rd16(dce + 4), 0x6F24);
    CHECK_EQ(acc_run(DRIVER_ATA_CD, -55), 0);
    CHECK_EQ(events, e0 + 1);
    /* Reads are PACKET commands (A0h) with READ(10); the disc's blocks of 512 bytes. */
    pb_rw(0, 10, 4 * 512, 1024, -55);
    CHECK_EQ(routine(DRIVER_ATA_CD, 10, -55), 0);
    uint8_t want[2048];
    CHECK(mc_vdisk_read(avcd, 128 + 4, 2, want));
    CHECK(!memcmp(ram + BUFFER, want, 1024));
    pb_rw(1, 10, 0, 512, -55);
    CHECK_EQ(routine(DRIVER_ATA_CD, 10, -55), -44);
    memset(ram + PB, 0, 80);
    wr16(PB + 22, 10);
    wr16(PB + 26, 23);
    CHECK_EQ(routine(DRIVER_ATA_CD, 12, -55), 0);
    CHECK_EQ(rd32(PB + 28), 0x0201);
    wr16(PB + 26, 43);
    wr32(PB + 28, 0x64657674);
    CHECK_EQ(routine(DRIVER_ATA_CD, 14, -55), 0);
    CHECK_EQ(rd32(PB + 32), 0x6364726D);        /* 'cdrm' */
    /* The disc taken out from the host side: the next poll finds it gone. */
    mc_vdisk *disc = mc_ata_cd_remove(&ata, 1);
    CHECK(disc == avcd);
    CHECK_EQ(acc_run(DRIVER_ATA_CD, -55), 0);
    CHECK_EQ(rd8(el - 3), 0);
    CHECK_EQ(events, e0 + 1);
    /* And back in: its unit attention, the same drive, a disk-inserted event. */
    CHECK(mc_ata_cd_insert(&ata, 1, disc));
    CHECK_EQ(acc_run(DRIVER_ATA_CD, -55), 0);
    CHECK_EQ(events, e0 + 2);
    CHECK_EQ(event_msg[e0 + 1], 10);
    CHECK_EQ(drive_el(10), el);
    CHECK_EQ(rd8(el - 3), 1);
    CHECK_EQ(ata.unit[1].scsi.block, 512);
    /* Eject: START STOP UNIT with LoEj. */
    wr16(PB + 22, 10);
    wr16(PB + 26, 7);
    CHECK_EQ(routine(DRIVER_ATA_CD, 12, -55), 0);
    CHECK_EQ(ata_ejected, 1);
    CHECK(ata.unit[1].scsi.media == NULL);
    CHECK_EQ(rd8(el - 3), 0);
    CHECK_EQ(acc_run(DRIVER_ATA_CD, -55), 0);
    CHECK_EQ(events, e0 + 2);
    /* The hard disk is still served. */
    pb_rw(0, 8, 7 * 512, 512, -54);
    CHECK_EQ(routine(DRIVER_ATA, 10, -54), 0);
    uint8_t one[512];
    CHECK(mc_disk_read(aimg1, 7, 1, one) && !memcmp(ram + BUFFER, one, 512));
    CHECK_EQ(exceptions, 0);
    CHECK_EQ(unknown_traps, 0);
    CHECK_EQ(ata_unknown, 0);
}

/* An ATA drive that answers neither identify command installs no drive. */
static void test_ata_absent(void) {
    mc_ata_attach_disk(&ata, 0, NULL);
    mc_ata_reset(&ata);
    hw_fake_step(&fk, MC_ATA_T_RESET + 1);
    setup_cpu();
    events = 0;
    load_descriptor(avd, DRIVER_ATA, 1, 0x0701);
    cpu.r[5] = 0;
    CHECK(call(DRIVER_ATA));
    CHECK(drive_el(8) == 0);
    CHECK_EQ(acc_run(DRIVER_ATA, -54), 0);
    CHECK_EQ(events, 0);
    CHECK_EQ(exceptions, 0);
}

int main(void) {
    int tests = 0;
    setup();
#define RUN(t) (t(), tests++)
    RUN(test_install);
    RUN(test_prime);
    RUN(test_control_status);
    RUN(test_scsi43);
    RUN(test_cd);
    RUN(test_adopt);
    setup_ata();
    RUN(test_ata_install);
    prime_suite(DRIVER_ATA, -54, aimg1, aimg2);
    tests++;
    RUN(test_ata_retry);
    RUN(test_ata_control_status);
    RUN(test_ata_cd);
    RUN(test_ata_absent);
    m68k_free(&cpu);
    mc_scsi_bus_free(&bus);
    mc_vdisk_destroy(vd);
    mc_vdisk_destroy(vcd);
    mc_disk_close(img1);
    mc_disk_close(img2);
    mc_disk_close(iso);
    mc_vdisk_destroy(avd);
    mc_vdisk_destroy(avcd);
    mc_disk_close(aimg1);
    mc_disk_close(aimg2);
    mc_disk_close(aiso);
    printf("test_livadixdisk: %d tests, %d failure(s)\n", tests, mc_test_failures);
    return mc_test_failures != 0;
}
