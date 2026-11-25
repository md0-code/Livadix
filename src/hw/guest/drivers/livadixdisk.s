| Livadix — livadixdisk, Livadix's own 68k block driver.
|
| Loaded by the Start Manager from the virtual disk's Apple_Driver43
| partition (src/hw/storage/vdisk.c) and called at its first byte with the
| SCSI ID in D5, or at +8 with a SCSI Manager 4.3 DeviceIdent in D5
| (Inside Macintosh: Devices, 4-11 "Loading and Initializing a Driver"). It
| installs itself in the unit table entry reserved for its ID (32 + ID),
| reads the partition map and adds one drive queue entry per Apple_HFS
| partition. Prime uses synchronous SCSIExecIO when SCSI Manager 4.3 is
| available (Devices chapter 4); older Systems use the original SCSI
| Manager calls (chapter 3). The code and its variables are one locked
| block in the system heap.
|
| Flavours: SCSI disks and SCSI CD-ROM drives (a CD is switched to 512-byte
| blocks with MODE SELECT, read-only, ejectable; the drive is polled for a
| new disc). The Start Manager loads a driver only from a disc, so the
| first Livadix driver installs a copy of itself for an empty CD-ROM drive at
| ID 3.
|
| The ATA flavour (5.1e) is this file assembled with ATA defined
| (livadixdisk_ata.s): the same driver for ATA hard disks and ATAPI CD-ROM
| drives through _ATAManager ($AAF1) for the Quadra 630 / LC 580 and the
| PowerBook 150/190, entered with the drive's ataPBDeviceID in D5. See the
| ATA section at the end.
|
| Written from Inside Macintosh: Devices (1994) chapter 1 "Device Manager"
| (driver header, DCE, DriverInstall, IODone, immediate calls), chapter 3
| "SCSI Manager" (SCSIDispatch selectors, TIB opcodes, partition map),
| chapter 4 (driver loading, SCSICreateRefNumXref), Inside Macintosh II
| (drive queue, AddDrive, DrvSts, low-memory globals), Inside Macintosh V
| "The Disk Driver" (control calls 21-23), and Designing Cards and Drivers
| for the Macintosh Family, 3rd ed. (driver routines and their registers).
|
| Assembler: GNU as for m68k (binutils), -m68000; built by
| tools/guest_build.py into src/hw/guest/bin/livadixdisk.bin.

| Low memory (Inside Macintosh II, "Summary of the Device Manager" and
| "File Manager"; IM IV for UTableBase).
        .set    UTableBase, 0x11C       | unit table: DCE handles
        .set    DrvQHead, 0x30A         | DrvQHdr+2: first drive queue element
        .set    JIODone, 0x8FC          | IODone vector
        .set    VCBQHead, 0x358         | VCBQHdr+2: first volume control block (IM IV)
        .set    vcbDrvNum, 72           | VCB: the drive a volume is on (IM IV, "The File Manager")
        .set    accRun, 65              | the periodic Control call of a dNeedTime driver (IM: Devices 1-...)

| Parameter block (IOParam/CntrlParam) and DCE offsets (IM: Devices 1-...).
        .set    ioTrap, 6
        .set    ioVRefNum, 22
        .set    csCode, 26
        .set    csParam, 28
        .set    ioBuffer, 32
        .set    ioReqCount, 36
        .set    ioActCount, 40
        .set    ioPosMode, 44
        .set    ioWPosOffset, 46        | XIOParam: 64-bit position when ioPosMode bit 8 is set (†)
        .set    dCtlDriver, 0
        .set    dCtlFlags, 4
        .set    dCtlPosition, 16
        .set    dCtlRefNum, 24
        .set    dCtlDelay, 34           | ticks between periodic calls
        .set    CD_ID, 3                | the SCSI ID of Apple's internal CD-ROM drive
        .set    POLL_TICKS, 60          | a CD drive looks for a new disc once a second
        .ifdef  ATA
        .set    MSEL_CDBLEN, 10         | ATAPI: MODE SELECT(10) with an 8-byte header (SFF-8020i)
        .set    MSEL_LEN, 16
        .else
        .set    MSEL_CDBLEN, 6
        .set    MSEL_LEN, 12
        .endif

| Result codes.
        .set    controlErr, -17
        .set    statusErr, -18
        .set    closErr, -24
        .set    ioErr, -36
        .set    wPrErr, -44
        .set    paramErr, -50
        .set    nsDrvErr, -56

| Variables (at `globals', the end of the image).
        .set    G_ID, 0                 | word: SCSI ID
        .set    G_REFNUM, 2             | word: driver reference number
        .set    G_DCE, 4                | long
        .set    G_DEVICE, 8             | long: DeviceIdent (SCSI Manager 4.3 entry)
        .set    G_NDRV, 12              | word: drives installed
        .set    G_CD, 14                | byte: $FF for a CD-ROM drive
        .set    G_MEDIA, 15             | byte: $FF while a medium is read
        .set    G_STAT, 16              | word: status from _SCSIComplete
        .set    G_MSG, 18               | word: message from _SCSIComplete
        .set    G_CDB, 20               | 12 bytes
        .set    G_TIB, 32               | two TIB instructions, 20 bytes
        .set    G_SENSE, 52             | 18 bytes of sense data
        .set    G_XREF, 70              | 44-byte SCSIDriverPB
        .set    G_BUF, 114              | one block
        .set    G_DRIVES, 626           | MAXDRV drive records
        .set    MAXDRV, 8
        .set    DR_FLAGS, 0             | the four flag bytes before a drive queue element
        .set    DR_QEL, 4               | DrvQEl: qLink, qType, dQDrive, dQRefNum, dQFSID, dQDrvSz, dQDrvSz2
        .set    DR_START, 20            | first block of the partition
        .set    DR_SIZE, 24             | blocks in it
        .set    DR_SIZEOF, 28
        .set    G_SLOT, 626 + 8 * 28    | word: drive records filled by the current scan
        .set    G_RUN, G_SLOT + 2       | byte: the first periodic call is done
        .set    G_ATTN, G_SLOT + 3      | byte: a unit attention was seen (a new medium)
        .set    G_BASE, G_SLOT + 4
        .ifdef  ATA
        .set    G_ATAPB, G_BASE         | 96 bytes: the ataExecIO parameter block
        .set    G_PKT, G_BASE + 96      | 18 bytes: ATAPICmdPacket (size word, 8 command words)
        .set    G_SIZE, G_BASE + 96 + 18
        .else
        .set    G_IOPTR, G_BASE         | SCSI Manager 4.3 request, allocated to the SIM's size
        .set    G_IOSIZE, G_BASE + 4    | word: scsiIOpbSize from SCSIBusInquiry
        .set    G_SIZE, G_BASE + 6
        .endif

        .text
start:
        .ifdef  ATA
        bra   install_ata             | +0: the ATA Manager's entry, D5 = ataPBDeviceID (unconfirmed)
        .short  0, 0
        bra   install_ata             | +8: the same
        .short  0, 0
        .else
        bra   install_old             | +0: the Start Manager's entry, D5 = SCSI ID
        .short  0, 0
        bra   install_new             | +8: SCSI Manager 4.3 entry, D5 = DeviceIdent
        .short  0, 0
        .endif

| The driver header (IM: Devices 1-...): flags dNeedLock, dNeedTime,
| dStatEnable, dCtlEnable, dWritEnable, dReadEnable; routine offsets; the
| name.
header:
        .short  0x6F00
        .short  0, 0, 0
        .short  drv_open - header
        .short  drv_prime - header
        .short  drv_control - header
        .short  drv_status - header
        .short  drv_close - header
        .ifdef  ATA
        .byte   11
        .ascii  ".LivadixATA"
        .else
        .byte   12
        .ascii  ".LivadixDisk"
        .endif
        .even

        .ifndef ATA
install_old:
        movem.l %d1-%d7/%a0-%a6,-(%sp)
        moveq   #7,%d0
        and.l   %d0,%d5
        move.l  #0x80000000,%d6        | old-call virtual ID: resolve when 4.3 is present
        bsr   install
        movem.l (%sp)+,%d1-%d7/%a0-%a6
        rts

install_new:
        movem.l %d1-%d7/%a0-%a6,-(%sp)
        move.l  %d5,%d6                 | DeviceIdent: reserved, bus, target ID, LUN
        move.l  %d5,%d0
        lsr.w   #8,%d0
        moveq   #7,%d5
        and.l   %d0,%d5
        bsr   install
        tst.w   %d0
        bne   1f
        bsr   register_xref
1:      movem.l (%sp)+,%d1-%d7/%a0-%a6
        rts
        .endif

| Installs the driver for SCSI ID D5 in unit 32 + ID and opens it (the ATA
| flavour: the first free unit from -54 down, D6 the ataPBDeviceID).
install:
        lea     globals(%pc),%a4
        move.w  %d5,G_ID(%a4)
        move.l  %d6,G_DEVICE(%a4)
        .ifndef ATA
        clr.l   G_IOPTR(%a4)           | an adopted copy must not share its maker's request
        clr.w   G_IOSIZE(%a4)
        .endif
        clr.b   G_RUN(%a4)              | a copy (adopt) starts with its maker's variables
        clr.b   G_ATTN(%a4)
        .ifdef  ATA
        bsr   pick_refnum
        .else
        moveq   #-33,%d0
        sub.w   %d5,%d0                 | refNum = -(33 + ID)
        .endif
        move.w  %d0,G_REFNUM(%a4)
        lea     header(%pc),%a0
        .short  0xA03D                  | _DrvrInstall: A0 driver, D0 refNum
        tst.w   %d0
        bne   1f
        move.w  G_REFNUM(%a4),%d0
        not.w   %d0                     | unit number
        ext.l   %d0
        lsl.l   #2,%d0
        movea.l UTableBase,%a0
        movea.l 0(%a0,%d0.l),%a0        | the DCE's handle
        .short  0xA029                  | _HLock
        movea.l (%a0),%a1
        move.l  %a1,G_DCE(%a4)
        lea     header(%pc),%a0
        move.l  %a0,dCtlDriver(%a1)     | a pointer: dRAMBased stays clear
        move.w  (%a0),%d0
        andi.w  #0xFF00,%d0
        .ifdef  ATA
        ori.w   #0x0024,%d0             | dOpened and DriverGestalt support†
        .else
        ori.w   #0x0020,%d0             | dOpened
        .endif
        move.w  %d0,dCtlFlags(%a1)
        move.w  G_REFNUM(%a4),dCtlRefNum(%a1)
        .ifdef  ATA
        bsr   ata_register
        .endif
        bsr   open_core
        tst.b   G_CD(%a4)
        beq   2f
        movea.l G_DCE(%a4),%a1
        move.w  #POLL_TICKS,dCtlDelay(%a1)
        bra   3f
2:
        .ifndef ATA
        bsr   adopt
        .endif
3:      moveq   #0,%d0
1:      rts

        .ifndef ATA
| An empty CD-ROM drive at ID 3 gets no driver from the Start Manager,
| which loads one only from a disc: a copy of this driver is installed for
| it, so a disc inserted later mounts. Nothing is done when the unit is
| taken, the device is not a CD-ROM drive or it holds a disc (its own
| driver loads from it).
adopt:
        movem.l %d3/%d6-%d7/%a3,-(%sp)
        moveq   #CD_ID,%d3
        move.w  G_ID(%a4),%d7
        cmp.w   %d3,%d7
        beq   9f
        moveq   #-33,%d0
        sub.w   %d3,%d0
        not.w   %d0                     | unit 32 + ID
        ext.l   %d0
        lsl.l   #2,%d0
        movea.l UTableBase,%a0
        tst.l   0(%a0,%d0.l)
        bne   9f                        | a driver is there
        move.l  G_DEVICE(%a4),%d6
        move.b  %d3,G_DEVICE+2(%a4)    | same bus, target 3 for the temporary probe
        move.w  %d3,G_ID(%a4)           | ask the device at ID 3
        lea     cdb_inquiry(%pc),%a0
        moveq   #6,%d0
        lea     G_BUF(%a4),%a1
        moveq   #36,%d1
        moveq   #0,%d2
        bsr   transact
        tst.w   %d0
        bne   8f                        | nobody there
        tst.w   G_STAT(%a4)
        bne   8f
        moveq   #0x1F,%d0
        and.b   G_BUF(%a4),%d0
        cmpi.b  #5,%d0
        bne   8f                        | not a CD-ROM drive
        lea     cdb_tur(%pc),%a0
        moveq   #6,%d0
        suba.l  %a1,%a1
        moveq   #0,%d1
        moveq   #0,%d2
        bsr   scsi_cmd
        beq   8f                        | a disc: its driver loads from it
        move.w  %d7,G_ID(%a4)
        move.l  %d6,G_DEVICE(%a4)
        move.l  #image_end - start,%d0
        .short  0xA51E                  | _NewPtrSys
        move.l  %a0,%d0
        beq   9f
        movea.l %a0,%a3
        movea.l %a0,%a1
        lea     start(%pc),%a0
        move.l  #image_end - start,%d0
        .short  0xA02E                  | _BlockMove (flushes the caches)
        tst.l   G_IOPTR(%a4)
        beq   1f
        move.l  G_DEVICE(%a4),%d5
        andi.l  #0x00FF0000,%d5         | the same bus, target 3, LUN 0
        move.w  %d3,%d0
        lsl.w   #8,%d0
        move.w  %d0,%d5
        jsr     8(%a3)                  | the SCSI Manager 4.3 entry
        bra   9f
1:      move.l  %d3,%d5
        jsr     (%a3)                   | the Start Manager's entry
        bra   9f
8:      move.w  %d7,G_ID(%a4)
        move.l  %d6,G_DEVICE(%a4)
9:      movem.l (%sp)+,%d3/%d6-%d7/%a3
        rts

| SCSI Manager 4.3: SCSICreateRefNumXref $85, SCSIDriverPB (44 bytes).
| IM: Devices 4-35, 4-49, and Universal Interfaces 3.4 SCSI.h:
| SCSIAction takes A0, returns D0, and selects _SCSIAtomic with D0 = 1.
register_xref:
        lea     G_XREF(%a4),%a0
        moveq   #10,%d0
1:      clr.l   (%a0)+
        dbra    %d0,1b
        lea     G_XREF(%a4),%a0
        move.w  #44,6(%a0)              | scsiPBLength
        move.b  #0x85,8(%a0)            | scsiFunctionCode
        move.l  G_DEVICE(%a4),12(%a0)   | scsiDevice
        move.w  G_REFNUM(%a4),36(%a0)   | scsiDriver
        moveq   #1,%d0
        .short  0xA089                  | SCSIAction(A0)
        rts

        .endif

| Identifies the device, sets 512-byte blocks, reads the partition map and
| adds a drive for each Apple_HFS partition. A4 = globals.
open_core:
        clr.w   G_NDRV(%a4)
        clr.b   G_MEDIA(%a4)
        .ifdef  ATA
        bsr   ata_identify              | G_CD: $FF for an ATAPI CD-ROM drive
        bne   1f
        bsr   scan_media
1:      rts
        .else
        lea     cdb_inquiry(%pc),%a0
        moveq   #6,%d0
        lea     G_BUF(%a4),%a1
        moveq   #36,%d1
        moveq   #0,%d2
        bsr   scsi_cmd
        bne   1f
        move.b  G_BUF(%a4),%d0
        andi.b  #0x1F,%d0
        cmpi.b  #5,%d0                  | peripheral device type 5: CD-ROM
        seq     G_CD(%a4)
        bsr   scan_media
1:      rts
        .endif

| Reads the medium: one drive per Apple_HFS partition, the records of an
| earlier medium used again first; records left over have no disk in
| place. G_MEDIA set when a medium was read.
scan_media:
        movem.l %d3-%d7/%a2-%a3,-(%sp)
        clr.w   G_SLOT(%a4)
        clr.b   G_MEDIA(%a4)
        .ifdef  ATA
        tst.b   G_CD(%a4)
        beq   2f                        | an ATA disk is always ready, in 512-byte blocks
        .endif
        lea     cdb_tur(%pc),%a0
        moveq   #6,%d0
        suba.l  %a1,%a1
        moveq   #0,%d1
        moveq   #0,%d2
        bsr   scsi_cmd
        bne   open_none               | no medium: no drives
        bsr   read_capacity
        bne   open_none
        cmpi.l  #512,G_BUF+4(%a4)
        beq   2f
        lea     cdb_modesel(%pc),%a0    | MODE SELECT: 512-byte blocks
        moveq   #MSEL_CDBLEN,%d0
        lea     mode_params(%pc),%a1
        moveq   #MSEL_LEN,%d1
        moveq   #1,%d2
        bsr   scsi_cmd
        bsr   read_capacity
        bne   open_none
        cmpi.l  #512,G_BUF+4(%a4)
        bne   open_none
2:      moveq   #0,%d0
        bsr   read_block
        bne   open_none
        cmpi.w  #0x4552,G_BUF(%a4)      | sbSIGWord
        bne   open_none
        st      G_MEDIA(%a4)
        moveq   #0,%d5                  | map blocks of 512 bytes: no shift
        cmpi.w  #2048,G_BUF+2(%a4)      | sbBlkSize 2048 (vdisk.h, the ATAPI layout):
        bne   6f                        | an entry per 2048 bytes, starts and
        moveq   #2,%d5                  | counts in 2048-byte blocks
6:      moveq   #1,%d3                  | map block
        moveq   #1,%d4                  | last map block
3:      move.l  %d3,%d0
        lsl.l   %d5,%d0
        bsr   read_block
        bne   open_none
        cmpi.w  #0x504D,G_BUF(%a4)      | pMapSIG
        bne   5f
        cmpi.l  #1,%d3
        bne   4f
        move.l  G_BUF+4(%a4),%d4        | pmMapBlkCnt
        cmpi.l  #63,%d4
        bls   4f
        moveq   #63,%d4
4:      bsr   is_hfs
        bne   5f
        bsr   add_drive
5:      addq.l  #1,%d3
        cmp.l   %d4,%d3
        bls   3b
open_none:
        move.w  G_SLOT(%a4),%d0
        move.w  %d0,%d1
        mulu    #DR_SIZEOF,%d1
        lea     G_DRIVES(%a4),%a0
        adda.l  %d1,%a0
1:      cmp.w   G_NDRV(%a4),%d0
        bcc   2f
        clr.b   DR_FLAGS+1(%a0)         | no disk in place
        lea     DR_SIZEOF(%a0),%a0
        addq.w  #1,%d0
        bra   1b
2:      moveq   #0,%d0
        movem.l (%sp)+,%d3-%d7/%a2-%a3
        rts

| Z set when the entry in G_BUF is an Apple_HFS partition.
is_hfs:
        lea     G_BUF+48(%a4),%a0       | pmParType
        lea     type_hfs(%pc),%a1
        moveq   #9,%d0
1:      cmpm.b  (%a0)+,(%a1)+
        dbne    %d0,1b
        rts

| The drive for the partition in G_BUF: the next record of an earlier
| medium, or a new drive queue element.
add_drive:
        move.w  G_SLOT(%a4),%d0
        cmpi.w  #MAXDRV,%d0
        bcc   3f
        mulu    #DR_SIZEOF,%d0
        lea     G_DRIVES(%a4),%a2
        adda.l  %d0,%a2
        move.l  G_BUF+8(%a4),%d0                | pmPyPartStart
        lsl.l   %d5,%d0                         | in 512-byte blocks (scan_media)
        move.l  %d0,DR_START(%a2)
        move.l  G_BUF+12(%a4),%d0               | pmPartBlkCnt
        lsl.l   %d5,%d0
        move.l  %d0,DR_SIZE(%a2)
        move.w  %d0,DR_QEL+12(%a2)              | dQDrvSz: low word
        swap    %d0
        move.w  %d0,DR_QEL+14(%a2)              | dQDrvSz2: high word
        clr.l   DR_FLAGS(%a2)
        move.b  #8,DR_FLAGS+1(%a2)              | nonejectable disk in place
        tst.b   G_CD(%a4)
        beq   1f
        move.b  #1,DR_FLAGS+1(%a2)              | ejectable disc in place
        bset    #7,DR_FLAGS(%a2)                | locked
        bra   2f
1:      move.l  G_BUF+88(%a4),%d0               | pmPartStatus: valid without the write bit
        btst    #0,%d0
        beq   2f
        btst    #5,%d0
        bne   2f
        bset    #7,DR_FLAGS(%a2)
2:      addq.w  #1,G_SLOT(%a4)
        move.w  G_SLOT(%a4),%d0
        cmp.w   G_NDRV(%a4),%d0
        bls   3f                                | a drive already in the queue
        clr.l   DR_QEL(%a2)                     | qLink
        move.w  #1,DR_QEL+4(%a2)                | qType 1: dQDrvSz2 is used
        clr.w   DR_QEL+10(%a2)                  | dQFSID: the File Manager's
        bsr   free_drive_number
        move.w  %d0,DR_QEL+6(%a2)               | dQDrive
        move.w  G_REFNUM(%a4),DR_QEL+8(%a2)     | dQRefNum
        lea     DR_QEL(%a2),%a0
        swap    %d0
        move.w  G_REFNUM(%a4),%d0
        .short  0xA04E                          | _AddDrive: A0 element, D0 drive number:refNum
        addq.w  #1,G_NDRV(%a4)
3:      rts

| The first drive number from 8 up that the drive queue does not use.
free_drive_number:
        moveq   #8,%d0
1:      movea.l DrvQHead,%a0
2:      move.l  %a0,%d1
        beq   3f
        cmp.w   6(%a0),%d0
        beq   4f
        movea.l (%a0),%a0
        bra   2b
4:      addq.w  #1,%d0
        bra   1b
3:      rts

| READ CAPACITY into G_BUF.
read_capacity:
        lea     cdb_capacity(%pc),%a0
        moveq   #10,%d0
        lea     G_BUF(%a4),%a1
        moveq   #8,%d1
        moveq   #0,%d2
        bra   scsi_cmd

| Device block D0 into G_BUF.
read_block:
        lea     G_BUF(%a4),%a1
        moveq   #1,%d1
        moveq   #0,%d2
| D1 blocks (at most $FFFF) at device block D0 from/to A1; D2 0 read, 1 write.
rw_blocks:
        .ifdef  ATA
        tst.b   G_CD(%a4)
        beq   ata_disk_rw               | an ATA disk: READ/WRITE SECTORS; an ATAPI drive: packets
        .endif
        lea     G_CDB(%a4),%a0
        move.b  #0x28,(%a0)             | READ(10)
        tst.w   %d2
        beq   1f
        move.b  #0x2A,(%a0)             | WRITE(10)
1:      clr.b   1(%a0)
        move.l  %d0,2(%a0)
        clr.b   6(%a0)
        move.b  %d1,8(%a0)
        ror.w   #8,%d1
        move.b  %d1,7(%a0)
        rol.w   #8,%d1
        clr.b   9(%a0)
        moveq   #9,%d0
        lsl.l   %d0,%d1
        moveq   #10,%d0
| One command with retries: A0 CDB, D0.W length, A1 buffer, D1.L bytes, D2.W
| 0 in / 1 out. Returns D0.W 0, ioErr or wPrErr (condition codes set).
scsi_cmd:
        movem.l %d3-%d6/%a2-%a3,-(%sp)
        movea.l %a0,%a2
        move.w  %d0,%d3
        movea.l %a1,%a3
        move.l  %d1,%d4
        move.w  %d2,%d5
        moveq   #3,%d6
1:      movea.l %a2,%a0
        move.w  %d3,%d0
        movea.l %a3,%a1
        move.l  %d4,%d1
        move.w  %d5,%d2
        bsr   transact
        tst.w   %d0
        bne   2f
        move.w  G_STAT(%a4),%d0
        andi.w  #0x3E,%d0               | status byte (SCSI-2 7.3)
        beq   9f
        cmpi.w  #2,%d0                  | CHECK CONDITION
        bne   2f
        bsr   request_sense
        cmpi.w  #6,%d0                  | UNIT ATTENTION (a new medium, a reset): try again
        bne   4f
        st      G_ATTN(%a4)
        bra   2f
4:
        cmpi.w  #7,%d0                  | DATA PROTECT
        beq   3f
        move.w  #ioErr,%d0
        bra   9f
3:      move.w  #wPrErr,%d0
        bra   9f
2:      subq.w  #1,%d6
        bne   1b
        move.w  #ioErr,%d0
9:      movem.l (%sp)+,%d3-%d6/%a2-%a3
        tst.w   %d0
        rts

| The sense key of the last error, or -1.
request_sense:
        lea     cdb_sense(%pc),%a0
        moveq   #6,%d0
        lea     G_SENSE(%a4),%a1
        moveq   #18,%d1
        moveq   #0,%d2
        bsr   transact
        tst.w   %d0
        bne   1f
        moveq   #15,%d0
        and.b   G_SENSE+2(%a4),%d0
        rts
1:      moveq   #-1,%d0
        rts

        .ifndef ATA
| One transaction through the SCSI Manager (Pascal calls of _SCSIDispatch,
| IM: Devices 3-31 to 3-41): get, select, command, read/write by TIB,
| complete. Returns D0.W the SCSI Manager result; G_STAT, G_MSG.
| IM: Devices 4-9, 4-11, 4-21 through 4-30 and 4-40 through 4-42:
| use a synchronous SCSIExecIO when 4.3 is available. Its driver loader
| can call a disk driver while it has blocked original SCSIDispatch calls.
| OS 8 System citt 46 "Main" +$8DE sets that guard while loading a driver;
| a legacy SCSIGet then returns scSequenceErr even though the bus is free.
| No Apple code or manager state is changed here. Old systems keep the TIB
| path. PB offsets, flags and Gestalt bits are Apple's Universal Interfaces
| SCSI.h / Gestalt.h (3.4); all reserved fields and completion are zero.
transact:
        movem.l %d0-%d2/%a0-%a1,-(%sp)
        bsr     scsi43_prepare
        tst.l   G_IOPTR(%a4)
        beq     1f
        movem.l (%sp)+,%d0-%d2/%a0-%a1
        bra     transact43
1:      movem.l (%sp)+,%d0-%d2/%a0-%a1
transact_old:
        movem.l %d3-%d5/%a2,-(%sp)
        movea.l %a0,%a2
        move.w  %d0,%d3
        move.l  %d1,%d4
        move.w  %d2,%d5
        lea     G_TIB(%a4),%a0
        move.w  #1,(%a0)                | scInc buffer, count
        move.l  %a1,2(%a0)
        move.l  %d1,6(%a0)
        move.w  #7,10(%a0)              | scStop
        clr.l   12(%a0)
        clr.l   16(%a0)
        move.w  #-1,G_STAT(%a4)
        clr.w   -(%sp)
        move.w  #1,-(%sp)               | SCSIGet
        .short  0xA815
        move.w  (%sp)+,%d0
        bne   9f
        clr.w   -(%sp)
        move.w  G_ID(%a4),-(%sp)
        move.w  #2,-(%sp)               | SCSISelect
        .short  0xA815
        move.w  (%sp)+,%d0
        bne   9f
        clr.w   -(%sp)
        move.l  %a2,-(%sp)
        move.w  %d3,-(%sp)
        move.w  #3,-(%sp)               | SCSICmd
        .short  0xA815
        move.w  (%sp)+,%d3
        bne   5f
        tst.l   %d4
        beq   5f
        clr.w   -(%sp)
        pea     G_TIB(%a4)
        move.w  #5,-(%sp)               | SCSIRead
        tst.w   %d5
        beq   4f
        move.w  #6,(%sp)                | SCSIWrite
4:      .short  0xA815
        move.w  (%sp)+,%d3
5:      clr.w   -(%sp)
        pea     G_STAT(%a4)
        pea     G_MSG(%a4)
        move.l  #300,-(%sp)             | five seconds
        move.w  #4,-(%sp)               | SCSIComplete
        .short  0xA815
        move.w  (%sp)+,%d0
        tst.w   %d3
        beq   9f
        move.w  %d3,%d0
9:      movem.l (%sp)+,%d3-%d5/%a2
        rts

| Query the actual SIM's request size once, not an assumed private suffix.
| A negative DeviceIdent marks a driver loaded by the original Start
| Manager. SCSIGetVirtualIDInfo resolves its virtual target to a bus/LUN.
scsi43_prepare:
        tst.l   G_IOPTR(%a4)
        bne     9f
        movem.l %d3/%a2-%a3,-(%sp)
| IM: Operating System Utilities, Gestalt Manager 1-5 and Trap Manager
| ch. 8: old ROMs need the trap-availability check before _Gestalt.
        move.w  #0xA1AD,%d0
        .short  0xA346                 | GetOSTrapAddress
        move.l  %a0,%d3
        move.w  #0xA89F,%d0
        .short  0xA746                 | GetToolboxTrapAddress(_Unimplemented)
        cmp.l   %a0,%d3
        beq     8f
        move.l  #0x73637369,%d0        | gestaltSCSI ('scsi')
        .short  0xA1AD
        tst.w   %d0
        bne     8f
        move.l  %a0,%d0
        btst    #0,%d0                 | gestaltAsyncSCSI
        beq     8f
        lea     -172(%sp),%sp          | sizeof SCSIBusInquiryPB
        movea.l %sp,%a2
        movea.l %sp,%a0
        moveq   #42,%d0
1:      clr.l   (%a0)+
        dbra    %d0,1b
        tst.l   G_DEVICE(%a4)
        bpl     3f
        move.w  #40,6(%a2)            | SCSIGetVirtualIDInfoPB
        move.b  #128,8(%a2)
        move.w  G_ID(%a4),36(%a2)
        movea.l %a2,%a0
        moveq   #1,%d0
        .short  0xA089                 | SCSIAction(A0), D0 result
        tst.w   %d0
        bne     7f
        tst.b   38(%a2)                | scsiExists
        beq     7f
        move.l  12(%a2),G_DEVICE(%a4)
3:      movea.l %a2,%a0
        moveq   #42,%d0
4:      clr.l   (%a0)+
        dbra    %d0,4b
        move.w  #172,6(%a2)
        move.b  #3,8(%a2)              | SCSIBusInquiry
        move.l  G_DEVICE(%a4),12(%a2)
        movea.l %a2,%a0
        moveq   #1,%d0
        .short  0xA089
        tst.w   %d0
        bne     7f
        moveq   #0,%d0
        move.w  44(%a2),%d0            | scsiIOpbSize, including SIM private storage
        cmpi.w  #176,%d0               | sizeof SCSI_IO
        bcs     7f
        move.w  %d0,G_IOSIZE(%a4)
        .short  0xA71E                 | NewPtrSysClear
        move.l  %a0,G_IOPTR(%a4)
7:      lea     172(%sp),%sp
8:      movem.l (%sp)+,%d3/%a2-%a3
9:      rts

transact43:
        movem.l %d3-%d6/%a2-%a3,-(%sp)
        movea.l %a0,%a2               | CDB
        movea.l %a1,%a3               | data
        move.w  %d0,%d3
        move.l  %d1,%d4
        move.w  %d2,%d5
        movea.l G_IOPTR(%a4),%a0
        move.w  G_IOSIZE(%a4),%d0
        subq.w  #1,%d0
1:      clr.b   (%a0)+
        dbra    %d0,1b
        movea.l G_IOPTR(%a4),%a0
        move.w  G_IOSIZE(%a4),6(%a0)
        move.b  #1,8(%a0)              | SCSIExecIO
        move.l  G_DEVICE(%a4),12(%a0)
        move.l  #0x61040000,%d6        | in, disable autosense, CDB pointer, no queue freeze
        tst.w   %d5
        beq     2f
        move.l  #0xA1040000,%d6        | out
2:      tst.l   %d4
        bne     3f
        move.l  #0xE1040000,%d6        | no data
3:      move.l  %d6,20(%a0)
        move.l  %a3,40(%a0)
        move.l  %d4,44(%a0)
        move.b  %d3,53(%a0)            | scsiCDBLength
        move.l  %a2,68(%a0)            | scsiCDB.cdbPtr
        move.l  #5000,84(%a0)          | positive Time Manager milliseconds
        moveq   #1,%d0
        .short  0xA089
        movea.l G_IOPTR(%a4),%a0
        clr.w   G_STAT(%a4)
        move.b  60(%a0),G_STAT+1(%a4)  | scsiSCSIstatus
        clr.w   G_MSG(%a4)
        tst.w   %d0
        beq     9f
        cmpi.w  #-7932,%d0             | scsiNonZeroStatus: outer layer handles REQUEST SENSE
        bne     9f
        moveq   #0,%d0
9:      movem.l (%sp)+,%d3-%d6/%a2-%a3
        rts

        .endif

| The drive record for the drive number in the parameter block (A2):
| A0, D0 = 0, or D0 = nsDrvErr.
find_drive:
        move.w  ioVRefNum(%a2),%d1
        move.w  G_NDRV(%a4),%d2
        lea     G_DRIVES(%a4),%a0
1:      subq.w  #1,%d2
        bmi   2f
        cmp.w   DR_QEL+6(%a0),%d1
        beq   3f
        lea     DR_SIZEOF(%a0),%a0
        bra   1b
2:      move.w  #nsDrvErr,%d0
        rts
3:      moveq   #0,%d0
        rts

| Device Manager routines: A0 parameter block, A1 DCE.
drv_open:
        moveq   #0,%d0                  | opened at installation
        rts

drv_close:
        move.w  #closErr,%d0            | a disk driver stays open
        rts

| Prime: partition-relative blocks from dCtlPosition (or the 64-bit
| position of a wide call), whole blocks only.
drv_prime:
        movem.l %d3-%d7/%a2-%a4,-(%sp)
        lea     globals(%pc),%a4
        movea.l %a0,%a2
        movea.l %a1,%a3
        bsr   find_drive
        bne   prime_out
        move.l  %a0,%d7
        btst    #0,ioPosMode(%a2)       | bit 8 of ioPosMode: wide positioning
        beq   1f
        move.l  ioWPosOffset(%a2),%d1   | high long
        move.l  ioWPosOffset+4(%a2),%d0
        cmpi.l  #0x200,%d1
        bcc   prime_param
        move.w  %d0,%d2
        andi.w  #0x1FF,%d2
        bne   prime_param
        moveq   #9,%d2
        lsr.l   %d2,%d0
        moveq   #23,%d2
        lsl.l   %d2,%d1
        or.l    %d1,%d0
        move.l  %d0,%d6
        bra   2f
1:      move.l  dCtlPosition(%a3),%d0
        move.w  %d0,%d2
        andi.w  #0x1FF,%d2
        bne   prime_param
        moveq   #9,%d2
        lsr.l   %d2,%d0
        move.l  %d0,%d6                 | first block in the partition
2:      move.l  ioReqCount(%a2),%d5
        move.w  %d5,%d2
        andi.w  #0x1FF,%d2
        bne   prime_param
        moveq   #9,%d2
        lsr.l   %d2,%d5                 | blocks
        movea.l %d7,%a0
        move.l  DR_SIZE(%a0),%d1
        cmp.l   %d1,%d6
        bhi   prime_range
        sub.l   %d6,%d1
        cmp.l   %d5,%d1
        bcs   prime_range
        move.w  ioTrap(%a2),%d4
        andi.w  #0xFF,%d4
        subq.w  #2,%d4                  | aRdCmd 2, aWrCmd 3
        beq   3f
        moveq   #1,%d4
        btst    #7,DR_FLAGS(%a0)
        bne   prime_locked
3:      move.l  ioBuffer(%a2),%d3
4:      tst.l   %d5
        beq   6f
        move.l  %d5,%d1
        cmpi.l  #128,%d1
        bls   5f
        move.l  #128,%d1
5:      move.l  %d1,-(%sp)
        movea.l %d7,%a0
        move.l  DR_START(%a0),%d0
        add.l   %d6,%d0
        movea.l %d3,%a1
        move.w  %d4,%d2
        bsr   rw_blocks
        move.l  (%sp)+,%d1
        tst.w   %d0
        bne   prime_out
        add.l   %d1,%d6
        sub.l   %d1,%d5
        moveq   #9,%d0
        lsl.l   %d0,%d1
        add.l   %d1,%d3
        bra   4b
6:      move.l  ioReqCount(%a2),%d0
        move.l  %d0,ioActCount(%a2)
        add.l   %d0,dCtlPosition(%a3)
        moveq   #0,%d0
        bra   prime_out
prime_param:
        move.w  #paramErr,%d0
        bra   prime_out
prime_range:
        move.w  #ioErr,%d0
        bra   prime_out
prime_locked:
        move.w  #wPrErr,%d0
prime_out:
        movea.l %a2,%a0
        movea.l %a3,%a1
        movem.l (%sp)+,%d3-%d7/%a2-%a4
| IODone unless the call was immediate (ioTrap bit 9, noQueueBit).
io_done:
        move.w  ioTrap(%a0),%d1
        btst    #9,%d1
        bne   1f
        move.l  JIODone,-(%sp)
1:      rts

| Control: killIO, verify and format (nothing to do), eject, the icons
| (21 media, 22 drive) and drive information (23).
drv_control:
        movem.l %d3-%d7/%a2-%a4,-(%sp)
        lea     globals(%pc),%a4
        movea.l %a0,%a2
        movea.l %a1,%a3
        moveq   #0,%d0
        move.w  csCode(%a2),%d3
        cmpi.w  #1,%d3                  | killIO
        beq   9f
        cmpi.w  #accRun,%d3
        beq   ctl_run
        bsr   find_drive
        bne   9f
        moveq   #0,%d0
        cmpi.w  #5,%d3                  | verify
        beq   9f
        cmpi.w  #6,%d3                  | format
        beq   9f
        cmpi.w  #7,%d3
        beq   ctl_eject
        lea     icon_disk(%pc),%a1
        tst.b   G_CD(%a4)
        beq   1f
        lea     icon_cd(%pc),%a1
1:      cmpi.w  #21,%d3                 | media icon, mask and "where" string
        beq   2f
        cmpi.w  #22,%d3                 | drive icon and mask
        bne   3f
2:      move.l  %a1,csParam(%a2)
        bra   9f
3:      cmpi.w  #23,%d3                 | drive information: unspecified type, SCSI, fixed
        bne   4f
        move.l  #0x0601,csParam(%a2)
        tst.b   G_CD(%a4)
        beq   9f
        move.l  #0x0201,csParam(%a2)    | removable
        bra   9f
4:      move.w  #controlErr,%d0
9:      movea.l %a2,%a0
        movea.l %a3,%a1
        movem.l (%sp)+,%d3-%d7/%a2-%a4
        bra   io_done
| Periodic calls. The first, once the system runs: a disk-inserted event
| for each of the driver's drives with a disk in place that no volume is
| mounted from (events posted during the start are flushed); a disk's
| driver then needs no more calls. A CD-ROM drive's driver keeps looking
| for a disc being inserted or taken out.
ctl_run:
        moveq   #0,%d3
        tst.b   G_CD(%a4)
        beq   1f
        bsr   poll_media
        move.w  %d0,%d3                 | 1: a new disc was announced
1:      tst.b   G_RUN(%a4)
        bne   4f
        st      G_RUN(%a4)
        tst.b   G_CD(%a4)
        bne   2f
        movea.l G_DCE(%a4),%a1
        andi.w  #0xDFFF,dCtlFlags(%a1)  | dNeedTime off
2:      tst.w   %d3
        bne   4f
        bsr   post_unmounted
4:      moveq   #0,%d0
        movea.l %a2,%a0
        movea.l %a3,%a1
        movem.l (%sp)+,%d3-%d7/%a2-%a4
        bra   io_done
| A disk-inserted event for each drive with a disk in place and no
| volume mounted from it.
post_unmounted:
        move.w  G_NDRV(%a4),%d2
        lea     G_DRIVES(%a4),%a1
1:      subq.w  #1,%d2
        bmi   4f
        tst.b   DR_FLAGS+1(%a1)
        beq   5f                        | no disk
        move.w  DR_QEL+6(%a1),%d1       | dQDrive
        movea.l VCBQHead,%a0
2:      move.l  %a0,%d0
        beq   3f                        | not mounted: ask for it
        cmp.w   vcbDrvNum(%a0),%d1
        beq   5f                        | mounted
        movea.l (%a0),%a0
        bra   2b
3:      moveq   #0,%d0
        move.w  %d1,%d0
        movea.w #7,%a0                  | diskEvt
        .short  0xA02F                  | _PostEvent
5:      lea     DR_SIZEOF(%a1),%a1
        bra   1b
4:      rts

| A CD-ROM drive: TEST UNIT READY. A disc that was not there, or one that
| raised a unit attention, is read and its volumes announced (D0 1); a
| disc gone leaves the drives without a disk (D0 0).
poll_media:
        clr.b   G_ATTN(%a4)
        lea     cdb_tur(%pc),%a0
        moveq   #6,%d0
        suba.l  %a1,%a1
        moveq   #0,%d1
        moveq   #0,%d2
        bsr   scsi_cmd
        bne   2f
        tst.b   G_MEDIA(%a4)
        beq   1f
        tst.b   G_ATTN(%a4)
        beq   8f                        | the same disc
1:      bsr   scan_media
        bsr   post_unmounted
        moveq   #1,%d0
        rts
2:      tst.b   G_MEDIA(%a4)
        beq   8f
        bsr   no_disk
8:      moveq   #0,%d0
        rts

| Every drive without a disk in place; no medium.
no_disk:
        clr.b   G_MEDIA(%a4)
        move.w  G_NDRV(%a4),%d1
        lea     G_DRIVES(%a4),%a0
1:      subq.w  #1,%d1
        bmi   2f
        clr.b   DR_FLAGS+1(%a0)
        lea     DR_SIZEOF(%a0),%a0
        bra   1b
2:      rts

ctl_eject:
        tst.b   G_CD(%a4)               | a fixed disk has nothing to eject
        beq   9b
        lea     cdb_eject(%pc),%a0      | START STOP UNIT: LoEj
        moveq   #6,%d0
        suba.l  %a1,%a1
        moveq   #0,%d1
        moveq   #0,%d2
        bsr   scsi_cmd
        bne   9b
        bsr   no_disk
        moveq   #0,%d0
        bra   9b

| Status: drive status (csCode 8, the DrvSts record of IM II).
drv_status:
        movem.l %d3-%d7/%a2-%a4,-(%sp)
        lea     globals(%pc),%a4
        movea.l %a0,%a2
        movea.l %a1,%a3
        move.w  #statusErr,%d0
        .ifdef  ATA
        cmpi.w  #43,csCode(%a2)         | driver gestalt (Designing PCI Cards and Drivers)
        beq   st_gestalt
        .endif
        cmpi.w  #8,csCode(%a2)
        bne   9f
        bsr   find_drive
        bne   9f
        lea     csParam(%a2),%a1
        clr.w   (%a1)                   | track
        move.b  DR_FLAGS(%a0),2(%a1)    | writeProt
        move.b  DR_FLAGS+1(%a0),3(%a1)  | diskInPlace
        move.b  #1,4(%a1)               | installed
        clr.b   5(%a1)                  | sides
        move.l  DR_QEL(%a0),6(%a1)      | qLink
        move.l  DR_QEL+4(%a0),10(%a1)   | qType, dQDrive
        move.l  DR_QEL+8(%a0),14(%a1)   | dQRefNum, dQFSID
        clr.l   18(%a1)                 | twoSideFmt, needsFlush, diskErrs
        moveq   #0,%d0
drv_status_out:
9:      movea.l %a2,%a0
        movea.l %a3,%a1
        movem.l (%sp)+,%d3-%d7/%a2-%a4
        bra   io_done

| Commands (SCSI-2).
cdb_inquiry:    .byte   0x12, 0, 0, 0, 36, 0
cdb_tur:        .byte   0x00, 0, 0, 0, 0, 0
cdb_sense:      .byte   0x03, 0, 0, 0, 18, 0
cdb_eject:      .byte   0x1B, 0, 0, 0, 0x02, 0
        .ifdef  ATA
cdb_modesel:    .byte   0x55, 0x10, 0, 0, 0, 0, 0, 0, 16, 0
        .else
cdb_modesel:    .byte   0x15, 0x10, 0, 0, 12, 0
        .endif
cdb_capacity:   .byte   0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0
        .ifdef  ATA
mode_params:    .byte   0, 0, 0, 0, 0, 0, 0, 8, 0, 0, 0, 0, 0, 0x00, 0x02, 0x00
        .else
mode_params:    .byte   0, 0, 0, 8, 0, 0, 0, 0, 0, 0, 0x02, 0x00
        .endif
type_hfs:       .ascii  "Apple_HFS\0"
        .even

| Livadix's own icons (32 x 32 ICN#: icon, mask), each followed by the empty
| "where" string of control call 21.
icon_disk:
        .long   0x00000000, 0x00000000, 0x00000000, 0x00000000
        .long   0x00000000, 0x00000000, 0x00000000, 0x00000000
        .long   0x00000000, 0x7FFFFFFE, 0x40000002, 0x40000002
        .long   0x44400002, 0x46C00002, 0x45400002, 0x44400002
        .long   0x44400002, 0x40000002, 0x5FFFFFFA, 0x40000002
        .long   0x40000072, 0x40000002, 0x7FFFFFFE, 0x00000000
        .long   0x00000000, 0x00000000, 0x00000000, 0x00000000
        .long   0x00000000, 0x00000000, 0x00000000, 0x00000000
        .long   0x00000000, 0x00000000, 0x00000000, 0x00000000
        .long   0x00000000, 0x00000000, 0x00000000, 0x00000000
        .long   0x00000000, 0x7FFFFFFE, 0x7FFFFFFE, 0x7FFFFFFE
        .long   0x7FFFFFFE, 0x7FFFFFFE, 0x7FFFFFFE, 0x7FFFFFFE
        .long   0x7FFFFFFE, 0x7FFFFFFE, 0x7FFFFFFE, 0x7FFFFFFE
        .long   0x7FFFFFFE, 0x7FFFFFFE, 0x7FFFFFFE, 0x00000000
        .long   0x00000000, 0x00000000, 0x00000000, 0x00000000
        .long   0x00000000, 0x00000000, 0x00000000, 0x00000000
        .byte   0
        .even
icon_cd:
        .long   0x00000000, 0x0003C000, 0x003FFC00, 0x00F00F00
        .long   0x01C00380, 0x030000C0, 0x06000060, 0x0C003030
        .long   0x18000818, 0x18000618, 0x3000020C, 0x3000010C
        .long   0x2007E084, 0x200C3084, 0x60081006, 0x60081006
        .long   0x60081006, 0x60081006, 0x200C3004, 0x2007E004
        .long   0x3000000C, 0x3000000C, 0x18000018, 0x18000018
        .long   0x0C000030, 0x06000060, 0x030000C0, 0x01C00380
        .long   0x00F00F00, 0x003FFC00, 0x0003C000, 0x00000000
        .long   0x00000000, 0x0003C000, 0x003FFC00, 0x00FFFF00
        .long   0x01FFFF80, 0x03FFFFC0, 0x07FFFFE0, 0x0FFFFFF0
        .long   0x1FFFFFF8, 0x1FFFFFF8, 0x3FFFFFFC, 0x3FFFFFFC
        .long   0x3FFFFFFC, 0x3FFFFFFC, 0x7FFFFFFE, 0x7FFFFFFE
        .long   0x7FFFFFFE, 0x7FFFFFFE, 0x3FFFFFFC, 0x3FFFFFFC
        .long   0x3FFFFFFC, 0x3FFFFFFC, 0x1FFFFFF8, 0x1FFFFFF8
        .long   0x0FFFFFF0, 0x07FFFFE0, 0x03FFFFC0, 0x01FFFF80
        .long   0x00FFFF00, 0x003FFC00, 0x0003C000, 0x00000000
        .byte   0
        .even

        .ifdef  ATA
| ----------------------------------------------------------------------------
| The ATA flavour. Written from Apple's ATA Device Software Guide (ATA Manager
| 3.0): the ATA Manager is the Toolbox trap $AAF1, called Pascal style (result
| word, then the parameter block pointer, on the stack); ataPBHdr (Guide p. 22:
| ataPBVers at 6, ataPBFunctionCode 18, ataPBFlags 20, ataPBDeviceID 24,
| ataPBTimeOut 28; 48 bytes), ataExecIO (ataStatusReg 48, ataErrorReg 49,
| BlindTxSize 52, ioBuffer 56, ioReqCount 60, the devicePB register image at
| 72: Features, Count, Sector, Reserved, Cylinder (big endian), SDH, Command;
| packetCDBPtr 80; 96 bytes), ATA_DrvrRegister ($85, drvrRefNum at 48),
| Table 3-1 for ataPBFlags (bits 4-5 ProtocolType, 12-13 data direction), and
| the ATAPICmdPacket (a size word, then the command bytes in order). Parameter
| block version 1 is used, which every ATA Manager supports. Bytes are moved
| in the order of the sector (ByteSwap clear, "LSB-MSB, compatible with PC
| clones"). The device's task file is addressed with LBA (SDH bit 6) and the
| DEV bit from ataPBDeviceID bits 15..8. The Quadra 630 ROM's ATA boot code
| (ATALOAD, ROM offset $D2880) enters the loaded driver at +0 with D5 = 0 (the
| master device) and A0 = an Apple_HFS map entry. † The unit table growth
| for unit 53, UnitNtryCnt at $1D2 and the layout of the driver gestalt
| parameter block are not documented in the Guide.
        .set    UnitNtryCnt, 0x1D2
        .set    ataPBVers, 6
        .set    ataPBFunctionCode, 18
        .set    ataPBFlags, 20
        .set    ataPBDeviceID, 24
        .set    ataPBTimeOut, 28
        .set    ataStatusReg, 48
        .set    ataDrvrRefNum, 48
        .set    ataBlindTxSize, 52
        .set    ataIOBuffer, 56
        .set    ataIOReqCount, 60
        .set    ataFeatures, 72
        .set    ataCount, 73
        .set    ataSector, 74
        .set    ataCylinder, 76
        .set    ataSDH, 78
        .set    ataCommand, 79
        .set    ataPacketPtr, 80
        .set    kATAMgrExecIO, 1
        .set    kATAMgrDrvrRegister, 0x85
        .set    mATAFlagIORead, 0x2000          | data direction in (bits 13..12 = 10)
        .set    mATAFlagIOWrite, 0x1000         | data direction out (01)
        .set    mATAFlagProtocolATAPI, 0x0020   | ProtocolType 10, as the G3 ROM's own packets ($2884CA)

| The entry: D5 = ataPBDeviceID (reserved word, devNum, busNum); D5 bit 0 of
| devNum is the DEV bit.
install_ata:
        movem.l %d1-%d7/%a0-%a6,-(%sp)
        move.l  %d5,%d6
        move.l  %d5,%d0                 | the G3 ROM's ATAPI loader gives a DeviceIdent
        swap    %d0                     | (bus in bits 23-16, device in 15-8, its
        andi.w  #0xFF,%d0               | flags in 7-0; ROM offset $287F96): a bus
        beq   1f                        | there is ataPBDeviceID's bus byte
        move.l  %d5,%d6
        andi.l  #0xFF00,%d6
        or.w    %d0,%d6
1:      move.l  %d6,%d0
        lsr.w   #8,%d0
        moveq   #1,%d5
        and.l   %d0,%d5
        bsr   install
        movem.l (%sp)+,%d1-%d7/%a0-%a6
        rts

| The first reference number from -54 down (the ATA disk driver's) whose unit
| table entry is free, D0.W; a unit past the table is free (DrvrInstall grows
| the table †).
pick_refnum:
        moveq   #-54,%d0
1:      move.w  %d0,%d1
        not.w   %d1                     | unit number
        cmp.w   UnitNtryCnt,%d1
        bcc   9f
        ext.l   %d1
        lsl.l   #2,%d1
        movea.l UTableBase,%a0
        tst.l   0(%a0,%d1.l)
        beq   9f
        subq.w  #1,%d0
        bra   1b
9:      rts

| ATA_DrvrRegister: this driver controls the device.
ata_register:
        bsr   ata_pb_init
        move.b  #kATAMgrDrvrRegister,ataPBFunctionCode(%a0)
        move.w  G_REFNUM(%a4),ataDrvrRefNum(%a0)
        bra   ata_call

| The ATA Manager with the parameter block at A0: D0.W the result.
ata_call:
        clr.w   -(%sp)
        move.l  %a0,-(%sp)
        .short  0xAAF1                  | _ATAManager
        move.w  (%sp)+,%d0
        rts

| A cleared ataExecIO block for the device (version 1, ATA_ExecIO, a
| 15-second time-out): A0.
ata_pb_init:
        lea     G_ATAPB(%a4),%a0
        movea.l %a0,%a1
        moveq   #23,%d0
1:      clr.l   (%a1)+
        dbra    %d0,1b
        move.b  #1,ataPBVers(%a0)
        move.b  #kATAMgrExecIO,ataPBFunctionCode(%a0)
        move.l  G_DEVICE(%a4),ataPBDeviceID(%a0)
        move.l  #15000,ataPBTimeOut(%a0)
        rts

| The SDH register image: LBA, and the DEV bit of the device ID: D0.B.
ata_sdh:
        move.l  G_DEVICE(%a4),%d0
        lsr.l   #4,%d0
        andi.b  #0x10,%d0
        ori.b   #0x40,%d0
        rts

| IDENTIFY DEVICE (EC), or IDENTIFY PACKET DEVICE (A1) when that is refused:
| G_CD is $FF for an ATAPI drive. D0.W 0 when the device answers.
ata_identify:
        clr.b   G_CD(%a4)
        moveq   #0,%d0
        move.b  #0xEC,%d0
        bsr   ata_ident_cmd
        beq   9f
        move.b  #0xA1,%d0
        bsr   ata_ident_cmd
        bne   9f
        st      G_CD(%a4)
        moveq   #0,%d0
9:      tst.w   %d0
        rts

| The identify command in D0.B into G_BUF.
ata_ident_cmd:
        move.w  %d0,%d1
        bsr   ata_pb_init
        move.w  #mATAFlagIORead,%d0
        cmpi.b  #0xA1,%d1
        bne   1f
        move.w  #mATAFlagIORead+mATAFlagProtocolATAPI,%d0
1:      move.w  %d0,ataPBFlags(%a0)
        move.l  #512,ataBlindTxSize(%a0)
        move.l  #512,ataIOReqCount(%a0)
        lea     G_BUF(%a4),%a1
        move.l  %a1,ataIOBuffer(%a0)
        move.b  %d1,ataCommand(%a0)
        bsr   ata_sdh
        lea     G_ATAPB(%a4),%a0
        move.b  %d0,ataSDH(%a0)
        bsr   ata_call
        tst.w   %d0
        rts

| An ATA disk: D1 blocks (at most 128) at LBA D0 from/to A1; D2 0 read, 1
| write. READ SECTORS (20) / WRITE SECTORS (30) with PIO, three tries.
| D0.W 0 or ioErr, condition codes set. Registers D3-D6 kept.
ata_disk_rw:
        movem.l %d3-%d6,-(%sp)
        move.l  %d0,%d3
        move.l  %d1,%d4
        move.w  %d2,%d5
        movea.l %a1,%a1
        move.l  %a1,-(%sp)
        moveq   #3,%d6
1:      bsr   ata_pb_init
        movea.l (%sp),%a1
        move.l  %a1,ataIOBuffer(%a0)
        move.w  #mATAFlagIORead,%d0
        tst.w   %d5
        beq   2f
        move.w  #mATAFlagIOWrite,%d0
2:      move.w  %d0,ataPBFlags(%a0)
        move.l  #512,ataBlindTxSize(%a0)
        move.l  %d4,%d0
        lsl.l   #8,%d0
        add.l   %d0,%d0
        move.l  %d0,ataIOReqCount(%a0)
        move.b  %d4,ataCount(%a0)
        move.b  %d3,ataSector(%a0)      | LBA 7..0
        move.l  %d3,%d0
        lsr.l   #8,%d0
        move.w  %d0,ataCylinder(%a0)    | LBA 23..8
        bsr   ata_sdh
        move.l  %d3,%d1
        moveq   #24,%d2
        lsr.l   %d2,%d1
        andi.b  #0x0F,%d1
        or.b    %d1,%d0                 | LBA 27..24
        lea     G_ATAPB(%a4),%a0
        move.b  %d0,ataSDH(%a0)
        moveq   #0x20,%d0               | READ SECTORS
        tst.w   %d5
        beq   3f
        moveq   #0x30,%d0               | WRITE SECTORS
3:      move.b  %d0,ataCommand(%a0)
        bsr   ata_call
        tst.w   %d0
        beq   9f
        subq.w  #1,%d6
        bne   1b
        move.w  #ioErr,%d0
9:      addq.l  #4,%sp
        movem.l (%sp)+,%d3-%d6
        tst.w   %d0
        rts

| One packet command of an ATAPI drive (the SCSI Manager's transact): A0 CDB,
| D0.W its length, A1 buffer, D1.L bytes, D2.W 0 in / 1 out. The command is
| sent as a 12-byte ATAPI packet, zero padded (SFF-8020i). D0.W 0 when the
| command ran, G_STAT 0 (good) or 2 (the device reported an error: check
| condition); another result is a transport error.
transact:
        movem.l %d3-%d5/%a2-%a3,-(%sp)
        movea.l %a0,%a2
        move.w  %d0,%d3
        movea.l %a1,%a3
        move.l  %d1,%d4
        move.w  %d2,%d5
        lea     G_PKT(%a4),%a0
        move.w  #12,(%a0)+              | packet size
        moveq   #15,%d0
1:      clr.b   (%a0)+
        dbra    %d0,1b
        lea     G_PKT+2(%a4),%a0
        subq.w  #1,%d3
2:      move.b  (%a2)+,(%a0)+
        dbra    %d3,2b
        bsr   ata_pb_init
        move.w  #mATAFlagProtocolATAPI,%d0
        tst.l   %d4
        beq   3f
        ori.w   #mATAFlagIORead,%d0
        tst.w   %d5
        beq   3f
        eori.w  #mATAFlagIORead+mATAFlagIOWrite,%d0
3:      move.w  %d0,ataPBFlags(%a0)
        move.w  #0x800,ataCylinder(%a0) | at most 2048 bytes for each DRQ
        move.l  #0x800,ataBlindTxSize(%a0)  | as the G3 ROM's own packets ($288460)
        move.l  %d4,ataIOReqCount(%a0)
        move.l  %a3,ataIOBuffer(%a0)
        lea     G_PKT(%a4),%a1
        move.l  %a1,ataPacketPtr(%a0)
        move.b  #0xA0,ataCommand(%a0)   | PACKET
        bsr   ata_sdh
        lea     G_ATAPB(%a4),%a0
        move.b  %d0,ataSDH(%a0)
        bsr   ata_call
        move.w  #-1,G_STAT(%a4)
        tst.w   %d0
        bne   4f
        clr.w   G_STAT(%a4)
        bra   9f
4:      lea     G_ATAPB(%a4),%a0
        btst    #0,ataStatusReg(%a0)    | ERR in the status register image
        beq   9f
        move.w  #2,G_STAT(%a4)
        moveq   #0,%d0
9:      movem.l (%sp)+,%d3-%d5/%a2-%a3
        rts

| Status 43, driver gestalt (the selectors of Designing PCI Cards and Drivers
| †): the answer for the selector in csParam is stored after it.
| This identifies the driver, not a mounted drive number.
st_gestalt:
        move.l  csParam(%a2),%d1
        lea     gestalt_table(%pc),%a0
1:      move.l  (%a0)+,%d2
        beq   2f
        move.l  (%a0)+,%d3
        cmp.l   %d1,%d2
        bne   1b
        cmpi.l  #0x64657674,%d1         | 'devt'
        bne   3f
        tst.b   G_CD(%a4)
        beq   3f
        move.l  #0x6364726D,%d3         | 'cdrm'
3:      move.l  %d3,csParam+4(%a2)
        moveq   #0,%d0
        bra   drv_status_out
2:      move.w  #statusErr,%d0
        bra   drv_status_out
gestalt_table:
        .long   0x73796E63, 1           | 'sync': synchronous
        .long   0x64657674, 0x6469736B  | 'devt': 'disk'
        .long   0x696E7466, 0x69646520  | 'intf': 'ide '
        .long   0x76657273, 0x0100      | 'vers'
        .long   0x6C707772, 0           | 'lpwr': no power mode control
        .long   0x70757267, 0           | 'purg': stays loaded
        .long   0x77696465, 1           | 'wide': 64-bit positioning
        .long   0x656A6563, 0           | 'ejec': eject on restart and shutdown
        .long   0
        .endif

| Variables: part of the loaded image, so they live in the same locked
| block as the code.
globals:
        .space  G_SIZE
image_end:
