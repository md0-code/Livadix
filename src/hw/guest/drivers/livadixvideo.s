| Livadix — livadixvideo, the 68k video driver of the Livadix Display Card
| (the card's registers in src/hw/livadix_card.h).
|
| Lives in the card's declaration ROM as the sDriver of every video
| sResource (src/hw/guest/declrom/livadixcard.yaml); the Slot Manager loads it
| into the system heap and the Start Manager opens it for the screen. It is
| position-independent: every reference to itself is PC-relative.
|
| Open allocates the private storage (a locked handle in dCtlStorage),
| reads the mode the card is in, loads a linear gamma table and installs the
| slot interrupt handler, which acknowledges the card's vertical blank and
| runs the slot's VBL tasks (the cursor's, on the main screen). Control and
| Status are the video driver calls of Designing Cards and Drivers for the
| Macintosh Family, 3rd ed., chapter 9 "Video driver routines" (Control 0-9:
| Reset, KillIO, SetMode, SetEntries, SetGamma, GrayScreen, SetGray,
| SetInterrupt, DirectSetEntries, SetDefaultMode; Status 2-9: GetMode,
| GetEntries, GetPages, GetBaseAddr, GetGray, GetInterrupt, GetGamma,
| GetDefaultMode), and the Display Manager's calls (Control 10
| SwitchMode, 16 SavePreferredConfiguration; Status 10 GetCurMode, 12
| GetConnection, 13 GetModeTiming, 16 GetPreferredConfiguration, 17
| GetNextResolution, 18 GetVideoParameters; their records as Apple's video
| driver interface defines them†). Every call completes at once. The
| resolution saved by SetDefaultMode or SavePreferredConfiguration is the
| one the card's PrimaryInit starts in next time (livadixcard_init.s).
|
| The card is reached at its 32-bit address (dCtlDevBase, f32BitMode): the
| driver switches the MMU to 32-bit mode around each access (_SwapMMUMode)
| and strips the pointers it is given in the caller's mode first
| (_StripAddress), as chapter 9 advises for GrayScreen. Assembled with
| CARD24 (livadixvideo24.s, for livadixcard24.yaml) the sResource is not
| f32BitMode and dCtlDevBase is the slot's 24-bit form $Fss0 0000 (chapter
| 8, "Identifying 32-bit addressable configurations"): its top byte gives
| the card's 32-bit address.
|
| Sources: Designing Cards and Drivers, 3rd ed., chapter 9 ("Video
| drivers", the parameter records, "Slot device interrupts": the slot
| queue element and _SIntInstall/_SIntRemove, the interrupt handler's
| registers and result; "Gamma correction in Macintosh computers": the
| gamma table record), chapter 8 (slot PRAM); Inside Macintosh V ("The
| Vertical Retrace Manager": DoVBLTask through jDoVBLTask at $D28; trap
| numbers), Inside Macintosh: Devices (1994) chapter 1 (driver header, DCE,
| IODone) and chapter 2 (Slot Manager selectors $11 SReadPRAMRec and $12
| SPutPRAMRec, the SpBlock and SPRAMRecord layouts).
|
| Assembler: GNU as for m68k (binutils), -m68000; built by
| tools/guest_build.py into src/hw/guest/bin/livadixvideo.bin.

| Traps.
        .set    _HLock, 0xA029
        .set    _DisposeHandle, 0xA023
        .set    _NewHandleSysClear, 0xA722
        .set    _StripAddress, 0xA055
        .set    _SwapMMUMode, 0xA05D
        .set    _SlotManager, 0xA06E
        .set    _SIntInstall, 0xA075
        .set    _SIntRemove, 0xA076

| Low memory.
        .set    JIODone, 0x8FC
        .set    JVBLTask, 0xD28

| Parameter block and DCE.
        .set    ioTrap, 6
        .set    csCode, 26
        .set    csParam, 28
        .set    dCtlStorage, 20
        .set    dCtlSlot, 40
        .set    dCtlSlotId, 41
        .set    dCtlDevBase, 42

| Results.
        .set    controlErr, -17
        .set    statusErr, -18
        .set    memFullErr, -108

| The card (src/hw/livadix_card.h).
        .set    REGS, 0xC00000
        .set    R_VRAM, 0x008
        .set    R_MONITOR, 0x00C
        .set    R_MODES, 0x010
        .set    R_WIDTH, 0x020
        .set    R_HEIGHT, 0x024
        .set    R_DEPTH, 0x028
        .set    R_STRIDE, 0x02C
        .set    R_BASE, 0x030
        .set    FB_BASE, 0x040          | MinorBaseOS: 64 bytes into VRAM
        .set    R_CONTROL, 0x040
        .set    R_STATUS, 0x044
        .set    R_CLUT_INDEX, 0x050
        .set    R_CLUT_DATA, 0x054
        .set    R_GAMMA_INDEX, 0x058
        .set    R_GAMMA_DATA, 0x05C
        .set    R_RES, 0x100
        .set    FIRST_MODE, 128         | the 1-bit depth mode; 133 is 32 bits
        .set    FIRST_RES, 128          | the video sResource of resolution 0

| Slot Manager parameter block (Devices 2-23) and selectors.
        .set    spResult, 0
        .set    spsPointer, 4
        .set    spSlot, 49
        .set    spExtDev, 51
        .set    spBlockSize, 56
        .set    sReadPRAMRec, 0x11
        .set    SetSRsrcState, 0x09
        .set    spID, 50
        .set    spParamData, 24
        .set    sPutPRAMRec, 0x12

| Private storage.
        .set    S_BASE, 0               | long: VRAM
        .set    S_REGS, 4               | long: the registers
        .set    S_DCE, 8                | long
        .set    S_MODE, 12              | word: depth mode, 128-133
        .set    S_DEPTH, 14             | word: bits per pixel
        .set    S_RES, 16               | word: resolution index
        .set    S_WIDTH, 18             | word
        .set    S_HEIGHT, 20            | word
        .set    S_STRIDE, 22            | word
        .set    S_FLAGS, 24             | byte: bit 0 luminance mapping, 1 VBL off, 2 direct depth
        .set    S_SLOT, 25              | byte
        .set    S_SQ, 26                | 16 bytes: slot interrupt queue element
        .set    S_VRAM, 42              | long: VRAM size
        .set    S_MODES, 46             | word: resolutions
        .set    S_MMU, 48               | byte: the caller's MMU mode
        .set    S_GAMMA, 50             | gamma table: 12-byte header, three channels of 256
        .set    S_PRAM, 50 + 12 + 768   | 8 bytes: the slot's PRAM record
        .set    S_SPB, S_PRAM + 8       | a Slot Manager parameter block
        .set    S_SIZE, S_SPB + spBlockSize
        .set    F_GRAY, 0
        .set    F_VBLOFF, 1
        .set    F_DIRECT, 2

        .text
| The driver header (Devices 1-...): dNeedLock, dStatEnable, dCtlEnable.
header:
        .short  0x4C00
        .short  0, 0, 0
        .short  drv_open - header
        .short  0
        .short  drv_control - header
        .short  drv_status - header
        .short  drv_close - header
        .byte   28
        .ascii  ".Display_Video_Apple_Livadix"
        .even

| --- open and close ---------------------------------------------------------

drv_open:
        movem.l %d3-%d7/%a2-%a4,-(%sp)
        movea.l %a1,%a4
        move.l  #S_SIZE,%d0
        .short  _NewHandleSysClear
        tst.w   %d0
        bne     9f
        move.l  %a0,dCtlStorage(%a4)
        .short  _HLock
        move.l  (%a0),%d0
        .short  _StripAddress
        movea.l %d0,%a3
        move.l  %a4,%d0
        .short  _StripAddress
        move.l  %d0,S_DCE(%a3)
        move.b  dCtlSlot(%a4),S_SLOT(%a3)
        move.l  dCtlDevBase(%a4),%d0
        move.l  %d0,S_BASE(%a3)
        andi.l  #0xFF000000,%d0         | $Fs00 0040 / $Fss0 0040 to $Fs00 0000
        addi.l  #REGS,%d0
        move.l  %d0,S_REGS(%a3)
        bsr     hw32
        movea.l S_REGS(%a3),%a2
        move.l  R_VRAM(%a2),S_VRAM(%a3)
        move.w  R_MODES+2(%a2),S_MODES(%a3)
        move.w  R_WIDTH+2(%a2),S_WIDTH(%a3)
        move.w  R_HEIGHT+2(%a2),S_HEIGHT(%a3)
        move.w  R_STRIDE+2(%a2),S_STRIDE(%a3)
        | The resolution the card shows: its entry in the RES table.
        move.w  S_WIDTH(%a3),%d0
        swap    %d0
        move.w  S_HEIGHT(%a3),%d0
        lea     R_RES(%a2),%a0
        moveq   #0,%d1
1:      cmp.w   S_MODES(%a3),%d1
        bge     2f
        cmp.l   (%a0)+,%d0
        beq     2f
        addq.w  #1,%d1
        bra     1b
2:      move.w  %d1,S_RES(%a3)
        | The depth and its mode.
        move.l  R_DEPTH(%a2),%d0
        move.w  %d0,S_DEPTH(%a3)
        move.w  #FIRST_MODE,%d1
3:      lsr.w   #1,%d0
        beq     4f
        addq.w  #1,%d1
        bra     3b
4:      move.w  %d1,S_MODE(%a3)
        cmpi.w  #16,S_DEPTH(%a3)
        bcs     5f
        bset    #F_DIRECT,S_FLAGS(%a3)
5:      bsr     gamma_linear
        bsr     gamma_load
        bsr     hw_back
        bsr     vbl_on
        moveq   #0,%d0
9:      movem.l (%sp)+,%d3-%d7/%a2-%a4
        rts

| Close: interrupts off, the display off, the storage freed.
drv_close:
        movem.l %d3-%d7/%a2-%a4,-(%sp)
        movea.l %a1,%a4
        move.l  dCtlStorage(%a4),%d0
        beq     9f
        movea.l %d0,%a0
        move.l  (%a0),%d0
        .short  _StripAddress
        movea.l %d0,%a3
        btst    #F_VBLOFF,S_FLAGS(%a3)
        bne     1f
        bsr     vbl_off
1:      bsr     hw32
        movea.l S_REGS(%a3),%a0
        clr.l   R_CONTROL(%a0)
        bsr     hw_back
        movea.l dCtlStorage(%a4),%a0
        .short  _DisposeHandle
        clr.l   dCtlStorage(%a4)
9:      moveq   #0,%d0
        movem.l (%sp)+,%d3-%d7/%a2-%a4
        rts

| --- the MMU mode -----------------------------------------------------------

| To 32-bit mode, keeping the caller's (A3 storage).
hw32:
        move.l  %d0,-(%sp)
        moveq   #1,%d0
        .short  _SwapMMUMode
        move.b  %d0,S_MMU(%a3)
        move.l  (%sp)+,%d0
        rts

| Back to the caller's mode.
hw_back:
        move.l  %d0,-(%sp)
        move.b  S_MMU(%a3),%d0
        .short  _SwapMMUMode
        move.l  (%sp)+,%d0
        rts

| D0 = a pointer in the caller's mode, stripped.
strip:
        .short  _StripAddress
        rts

| --- the vertical blank -----------------------------------------------------

| Installs the slot interrupt handler and lets the card interrupt.
vbl_on:
        lea     S_SQ(%a3),%a0
        move.w  #6,4(%a0)               | sqType: sIQType
        clr.w   6(%a0)                  | sqPrio
        lea     vbl_irq(%pc),%a1
        move.l  %a1,8(%a0)              | sqAddr
        move.l  %a3,12(%a0)             | sqParm: the storage, for A1
        moveq   #0,%d0
        move.b  S_SLOT(%a3),%d0
        .short  _SIntInstall
        bclr    #F_VBLOFF,S_FLAGS(%a3)
        bsr     hw32
        movea.l S_REGS(%a3),%a0
        move.l  #1,R_STATUS(%a0)
        move.l  #3,R_CONTROL(%a0)       | display on, VBL interrupts
        bsr     hw_back
        rts

vbl_off:
        bsr     hw32
        movea.l S_REGS(%a3),%a0
        move.l  #1,R_CONTROL(%a0)
        move.l  #1,R_STATUS(%a0)
        bsr     hw_back
        lea     S_SQ(%a3),%a0
        moveq   #0,%d0
        move.b  S_SLOT(%a3),%d0
        .short  _SIntRemove
        bset    #F_VBLOFF,S_FLAGS(%a3)
        rts

| The slot interrupt handler: A1 = the storage. Acknowledges the card's
| vertical blank and runs the slot's VBL tasks; D0 = 1 when the interrupt
| was the card's. Registers but D0 and A1 are kept.
vbl_irq:
        movem.l %d1-%d2/%a0/%a2,-(%sp)
        movea.l %a1,%a2
        moveq   #1,%d0
        .short  _SwapMMUMode
        move.l  %d0,%d2
        movea.l S_REGS(%a2),%a0
        move.l  R_STATUS(%a0),%d1
        btst    #0,%d1
        beq     1f
        move.l  #1,R_STATUS(%a0)
        move.l  %d2,%d0
        .short  _SwapMMUMode
        moveq   #0,%d0
        move.b  S_SLOT(%a2),%d0
        movea.l JVBLTask,%a0
        jsr     (%a0)
        moveq   #1,%d0
        bra     2f
1:      move.l  %d2,%d0
        .short  _SwapMMUMode
        moveq   #0,%d0
2:      movem.l (%sp)+,%d1-%d2/%a0/%a2
        rts

| --- gamma ------------------------------------------------------------------

| The storage's table header: three channels of 256 bytes, no formula.
gamma_header:
        lea     S_GAMMA(%a3),%a1
        clr.l   (%a1)+                  | gVersion, gType
        clr.w   (%a1)+                  | gFormulaSize
        move.w  #3,(%a1)+               | gChanCnt
        move.w  #256,(%a1)+             | gDataCnt
        move.w  #8,(%a1)+               | gDataWidth
        rts

| A linear table in the storage.
gamma_linear:
        bsr     gamma_header
        movea.l %a1,%a0
        moveq   #2,%d1
1:      moveq   #0,%d0
2:      move.b  %d0,(%a0)+
        addq.b  #1,%d0
        bne     2b
        dbra    %d1,1b
        rts

| The storage's table into the card (in 32-bit mode).
gamma_load:
        movea.l S_REGS(%a3),%a0
        lea     S_GAMMA+12(%a3),%a1
        clr.l   R_GAMMA_INDEX(%a0)
        move.w  #255,%d1
        moveq   #0,%d0
1:      move.b  (%a1),%d0
        move.l  %d0,R_GAMMA_DATA(%a0)
        move.b  256(%a1),%d0
        move.l  %d0,R_GAMMA_DATA(%a0)
        move.b  512(%a1),%d0
        move.l  %d0,R_GAMMA_DATA(%a0)
        addq.l  #1,%a1
        dbra    %d1,1b
        rts

| --- control ----------------------------------------------------------------

| A0 = parameter block, A1 = DCE. Handlers run with A4 = the parameter
| block, A3 = the storage, A2 = csParam, all stripped; D0 = the result.
drv_control:
        movem.l %d3-%d7/%a2-%a4,-(%sp)
        bsr     enter
        move.w  csCode(%a4),%d0
        cmpi.w  #16,%d0
        bhi     ctl_bad
        add.w   %d0,%d0
        move.w  ctl_table(%pc,%d0.w),%d0
        jmp     ctl_table(%pc,%d0.w)
ctl_table:
        .short  ctl_reset - ctl_table
        .short  ctl_good - ctl_table          | KillIO: nothing pending
        .short  ctl_setmode - ctl_table
        .short  ctl_setentries - ctl_table
        .short  ctl_setgamma - ctl_table
        .short  ctl_grayscreen - ctl_table
        .short  ctl_setgray - ctl_table
        .short  ctl_setinterrupt - ctl_table
        .short  ctl_directsetentries - ctl_table
        .short  ctl_setdefaultmode - ctl_table
        .short  ctl_switchmode - ctl_table
        .short  ctl_bad - ctl_table           | 11 SetSync
        .short  ctl_bad - ctl_table
        .short  ctl_bad - ctl_table
        .short  ctl_bad - ctl_table
        .short  ctl_bad - ctl_table
        .short  ctl_savepreferred - ctl_table

ctl_bad:
        move.w  #controlErr,%d0
        bra     leave
ctl_good:
        moveq   #0,%d0
        bra     leave

| The common entry: A4, A3, A2 set up (pointers stripped).
enter:
        move.l  %a0,%d0
        bsr     strip
        movea.l %d0,%a4
        movea.l dCtlStorage(%a1),%a0
        move.l  (%a0),%d0
        bsr     strip
        movea.l %d0,%a3
        move.l  csParam(%a4),%d0
        bsr     strip
        movea.l %d0,%a2
        rts

| The common exit: D0 the result; IODone unless the call was immediate.
leave:
        movea.l %a4,%a0
        movea.l S_DCE(%a3),%a1
        movem.l (%sp)+,%d3-%d7/%a2-%a4
        move.w  ioTrap(%a0),%d1
        btst    #9,%d1
        bne     1f
        move.l  JIODone,-(%sp)
1:      rts

| Reset: 1 bit, page 0.
ctl_reset:
        move.w  #FIRST_MODE,%d3
        bsr     set_depth
        bne     ctl_bad
        move.w  #FIRST_MODE,(%a2)       | csMode
        clr.w   6(%a2)                  | csPage
        move.l  S_BASE(%a3),8(%a2)      | csBaseAddr
        bra     ctl_good

| SetMode: csMode (a depth mode), csPage 0.
ctl_setmode:
        tst.w   6(%a2)
        bne     ctl_bad
        move.w  (%a2),%d3
        bsr     set_depth
        bne     ctl_bad
        move.l  S_BASE(%a3),8(%a2)
        bra     ctl_good

| SetEntries: indexed depths only.
ctl_setentries:
        btst    #F_DIRECT,S_FLAGS(%a3)
        bne     ctl_bad
        bsr     set_entries
        bra     leave

| DirectSetEntries: direct depths only.
ctl_directsetentries:
        btst    #F_DIRECT,S_FLAGS(%a3)
        beq     ctl_bad
        bsr     set_entries
        bra     leave

| SetGamma: csGTable, a gamma table of 1 or 3 channels of 256 bytes;
| NIL for a linear table. Takes effect at once (the card applies it after
| its CLUT).
ctl_setgamma:
        move.l  (%a2),%d0
        beq     6f
        bsr     strip
        movea.l %d0,%a0
        tst.w   (%a0)                   | gVersion 0
        bne     ctl_bad
        cmpi.w  #256,8(%a0)             | gDataCnt
        bne     ctl_bad
        cmpi.w  #8,10(%a0)              | gDataWidth
        bne     ctl_bad
        move.w  6(%a0),%d1              | gChanCnt
        cmpi.w  #1,%d1
        beq     1f
        cmpi.w  #3,%d1
        bne     ctl_bad
1:      move.w  4(%a0),%d0              | gFormulaSize
        lea     12(%a0,%d0.w),%a0       | the data
        bsr     gamma_header
        move.w  #255,%d2
        cmpi.w  #3,%d1
        beq     3f
2:      move.b  (%a0)+,%d0              | one channel for all three
        move.b  %d0,(%a1)
        move.b  %d0,256(%a1)
        move.b  %d0,512(%a1)
        addq.l  #1,%a1
        dbra    %d2,2b
        bra     7f
3:      move.w  #767,%d2
4:      move.b  (%a0)+,(%a1)+
        dbra    %d2,4b
        bra     7f
6:      bsr     gamma_linear
7:      bsr     hw32
        bsr     gamma_load
        bsr     hw_back
        bra     ctl_good

| GrayScreen: csPage 0 filled with the depth's 50% dither; in a direct
| depth the CLUT gets its linear ramps.
ctl_grayscreen:
        tst.w   6(%a2)
        bne     ctl_bad
        bsr     hw32
        bsr     gray_fill
        btst    #F_DIRECT,S_FLAGS(%a3)
        beq     1f
        bsr     clut_ramp
1:      bsr     hw_back
        bra     ctl_good

| SetGray: csMode 1 maps SetEntries colours to their luminance.
ctl_setgray:
        bclr    #F_GRAY,S_FLAGS(%a3)
        tst.b   (%a2)
        beq     ctl_good
        bset    #F_GRAY,S_FLAGS(%a3)
        bra     ctl_good

| SetInterrupt: csMode 0 enables the VBL interrupt, 1 disables it.
ctl_setinterrupt:
        tst.b   (%a2)
        bne     1f
        btst    #F_VBLOFF,S_FLAGS(%a3)
        beq     ctl_good
        bsr     vbl_on
        bra     ctl_good
1:      btst    #F_VBLOFF,S_FLAGS(%a3)
        bne     ctl_good
        bsr     vbl_off
        bra     ctl_good

| SetDefaultMode: the video sResource (spID) to start in, kept in the
| slot's PRAM (vendorUse2; vendorUse1 is the system's).
ctl_setdefaultmode:
        bsr     pram_read
        bne     ctl_bad
        move.b  (%a2),S_PRAM+3(%a3)     | vendorUse2
        bsr     pram_write
        bne     ctl_bad
        bra     ctl_good

| SavePreferredConfiguration: csMode (depth mode) and csData (display
| mode ID) of a VDSwitchInfoRec kept in the slot's PRAM as the start
| depth (vendorUse1) and resolution (vendorUse2).
ctl_savepreferred:
        move.l  2(%a2),%d1
        subi.l  #FIRST_RES,%d1
        bmi     ctl_bad
        cmp.w   S_MODES(%a3),%d1
        bcc     ctl_bad
        move.w  (%a2),%d3
        bsr     depth_fits
        bne     ctl_bad
        bsr     pram_read
        bne     ctl_bad
        move.b  1(%a2),S_PRAM+2(%a3)    | vendorUse1
        move.b  5(%a2),S_PRAM+3(%a3)    | vendorUse2
        bsr     pram_write
        bne     ctl_bad
        bra     ctl_good

| SwitchMode: csMode (depth mode), csData (the resolution's display mode
| ID, its video sResource 128 + n), csPage 0.
ctl_switchmode:
        tst.w   6(%a2)
        bne     ctl_bad
        move.l  2(%a2),%d0
        subi.l  #FIRST_RES,%d0
        bmi     ctl_bad
        cmp.w   S_MODES(%a3),%d0
        bcc     ctl_bad
        move.w  (%a2),%d3
        move.w  %d0,%d1
        bsr     depth_fits
        bne     ctl_bad
        | The new resolution's video sResource becomes the enabled one and
        | the DCE's (dCtlSlotId), where the system reads the mode's video
        | parameters from; the old one is disabled.
        move.w  S_RES(%a3),%d4
        cmp.w   %d4,%d1
        beq     1f
        move.w  %d1,%d0
        moveq   #0,%d2                  | enable
        bsr     srsrc_state
        move.w  %d4,%d0
        moveq   #1,%d2                  | disable
        bsr     srsrc_state
        movea.l S_DCE(%a3),%a0
        move.b  %d1,dCtlSlotId(%a0)
        addi.b  #FIRST_RES,dCtlSlotId(%a0)
1:      move.w  %d1,S_RES(%a3)
        bsr     hw32
        movea.l S_REGS(%a3),%a0
        add.w   %d1,%d1
        add.w   %d1,%d1
        addi.w  #R_RES,%d1
        move.l  0(%a0,%d1.w),%d0
        moveq   #0,%d2
        move.w  %d0,%d2
        move.w  %d2,S_HEIGHT(%a3)
        move.l  %d2,R_HEIGHT(%a0)
        swap    %d0
        move.w  %d0,%d2
        move.w  %d2,S_WIDTH(%a3)
        move.l  %d2,R_WIDTH(%a0)
        bsr     hw_back
        bsr     set_depth
        bne     ctl_bad
        move.l  S_BASE(%a3),8(%a2)
        bra     ctl_good

| --- status -----------------------------------------------------------------

drv_status:
        movem.l %d3-%d7/%a2-%a4,-(%sp)
        bsr     enter
        move.w  csCode(%a4),%d0
        cmpi.w  #18,%d0
        bhi     st_bad
        add.w   %d0,%d0
        move.w  st_table(%pc,%d0.w),%d0
        jmp     st_table(%pc,%d0.w)
st_table:
        .short  st_bad - st_table
        .short  st_bad - st_table
        .short  st_getmode - st_table
        .short  st_getentries - st_table
        .short  st_getpages - st_table
        .short  st_getbaseaddr - st_table
        .short  st_getgray - st_table
        .short  st_getinterrupt - st_table
        .short  st_getgamma - st_table
        .short  st_getdefaultmode - st_table
        .short  st_getcurmode - st_table
        .short  st_bad - st_table             | 11 GetSync
        .short  st_getconnection - st_table
        .short  st_getmodetiming - st_table
        .short  st_bad - st_table
        .short  st_bad - st_table
        .short  st_getpreferred - st_table
        .short  st_getnextresolution - st_table
        .short  st_getvideoparameters - st_table

st_bad:
        move.w  #statusErr,%d0
        bra     leave
st_good:
        moveq   #0,%d0
        bra     leave

| GetMode: csMode, csPage, csBaseAddr.
st_getmode:
        move.w  S_MODE(%a3),(%a2)
        clr.w   6(%a2)
        move.l  S_BASE(%a3),8(%a2)
        bra     st_good

| GetEntries: the CLUT entries asked for (sequential from csStart, or by
| their value fields when csStart is -1).
st_getentries:
        move.l  (%a2),%d0
        bsr     strip
        movea.l %d0,%a1                 | csTable
        move.w  4(%a2),%d4              | csStart
        move.w  6(%a2),%d5              | csCount (zero based)
        bsr     hw32
        movea.l S_REGS(%a3),%a0
1:      move.w  %d4,%d0
        bpl     2f
        move.w  (%a1),%d0
        bra     3f
2:      move.w  %d4,(%a1)
        addq.w  #1,%d4
3:      andi.l  #0xFF,%d0
        move.l  %d0,R_CLUT_INDEX(%a0)
        moveq   #2,%d2
        lea     2(%a1),%a1
4:      move.l  R_CLUT_DATA(%a0),%d0
        move.b  %d0,(%a1)+
        move.b  %d0,(%a1)+
        dbra    %d2,4b
        dbra    %d5,1b
        bsr     hw_back
        bra     st_good

| GetPages: one page.
st_getpages:
        move.w  #1,6(%a2)
        bra     st_good

| GetBaseAddr: page 0's address.
st_getbaseaddr:
        tst.w   6(%a2)
        bne     st_bad
        move.l  S_BASE(%a3),8(%a2)
        bra     st_good

st_getgray:
        moveq   #0,%d0
        btst    #F_GRAY,S_FLAGS(%a3)
        beq     1f
        moveq   #1,%d0
1:      move.b  %d0,(%a2)
        bra     st_good

st_getinterrupt:
        moveq   #0,%d0
        btst    #F_VBLOFF,S_FLAGS(%a3)
        beq     1f
        moveq   #1,%d0
1:      move.b  %d0,(%a2)
        bra     st_good

| GetGamma: csGTable, the driver's copy.
st_getgamma:
        lea     S_GAMMA(%a3),%a0
        move.l  %a0,(%a2)
        bra     st_good

| GetDefaultMode: the spID kept by SetDefaultMode, else the current one.
st_getdefaultmode:
        bsr     pram_read
        bne     1f
        move.b  S_PRAM+3(%a3),%d0       | vendorUse2
        bne     3f
1:      moveq   #0,%d0
        move.w  S_RES(%a3),%d0
        addi.w  #FIRST_RES,%d0
3:      move.b  %d0,(%a2)
        bra     st_good

| GetCurMode: csMode, csData (display mode ID), csPage, csBaseAddr.
st_getcurmode:
        move.w  S_MODE(%a3),(%a2)
        moveq   #0,%d0
        move.w  S_RES(%a3),%d0
        addi.l  #FIRST_RES,%d0
        move.l  %d0,2(%a2)
        clr.w   6(%a2)
        move.l  S_BASE(%a3),8(%a2)
        bra     st_good

| GetConnection: a VDDisplayConnectInfoRec: csDisplayType
| kMultiModeCRT3Connect (6, a multiple-mode display), no tagging, and
| csConnectFlags kAllModesValid | kAllModesSafe (bits 0 and 1): the
| display shows every mode the card offers.
st_getconnection:
        move.w  #6,(%a2)                | csDisplayType
        clr.w   2(%a2)                  | csConnectTaggedType, csConnectTaggedData
        move.l  #3,4(%a2)               | csConnectFlags
        clr.l   8(%a2)                  | csDisplayComponent
        clr.l   12(%a2)                 | csConnectReserved
        bra     st_good

| GetModeTiming: csTimingMode (a display mode ID) in; csTimingFormat
| 'decl', csTimingData timingInvalid (0: the card's own 60 Hz timing, no
| standard one) and csTimingFlags kModeValid | kModeSafe (bits 0, 1), with
| kModeDefault (bit 2) for the display's own resolution (the MONITOR
| register) out.
st_getmodetiming:
        move.l  (%a2),%d1
        subi.l  #FIRST_RES,%d1
        bmi     st_bad
        cmp.w   S_MODES(%a3),%d1
        bcc     st_bad
        move.l  #0x6465636C,8(%a2)      | csTimingFormat 'decl'
        clr.l   12(%a2)                 | csTimingData
        moveq   #3,%d2
        bsr     hw32
        movea.l S_REGS(%a3),%a0
        move.l  R_MONITOR(%a0),%d0
        bsr     hw_back
        cmp.w   %d0,%d1
        bne     1f
        moveq   #7,%d2
1:      move.l  %d2,16(%a2)             | csTimingFlags
        bra     st_good

| GetPreferredConfiguration: a VDSwitchInfoRec of the depth and resolution
| SavePreferredConfiguration kept (vendorUse1 and 2), else the current
| ones.
st_getpreferred:
        move.w  S_MODE(%a3),(%a2)
        moveq   #0,%d0
        move.w  S_RES(%a3),%d0
        addi.l  #FIRST_RES,%d0
        move.l  %d0,2(%a2)
        bsr     pram_read
        bne     2f
        moveq   #0,%d0
        move.b  S_PRAM+2(%a3),%d0
        cmpi.w  #FIRST_MODE,%d0
        bcs     1f
        cmpi.w  #FIRST_MODE+5,%d0
        bhi     1f
        move.w  %d0,(%a2)               | csMode
1:      moveq   #0,%d0
        move.b  S_PRAM+3(%a3),%d0
        subi.w  #FIRST_RES,%d0
        bmi     2f
        cmp.w   S_MODES(%a3),%d0
        bcc     2f
        addi.l  #FIRST_RES,%d0
        move.l  %d0,2(%a2)              | csData
2:      clr.w   6(%a2)                  | csPage
        move.l  S_BASE(%a3),8(%a2)      | csBaseAddr
        bra     st_good

| GetNextResolution: csPreviousDisplayModeID in (-2 the first, 0 the
| current); csDisplayModeID (-3 after the last), csHorizontalPixels,
| csVerticalLines, csRefreshRate, csMaxDepthMode out.
st_getnextresolution:
        move.l  (%a2),%d0
        cmpi.l  #-2,%d0
        bne     1f
        moveq   #0,%d1
        bra     3f
1:      tst.l   %d0
        bne     2f
        move.w  S_RES(%a3),%d1
        bra     3f
2:      subi.l  #FIRST_RES,%d0
        bmi     st_bad
        cmp.w   S_MODES(%a3),%d0
        bcc     st_bad
        move.w  %d0,%d1
        addq.w  #1,%d1
3:      cmp.w   S_MODES(%a3),%d1
        bcs     4f
        move.l  #-3,4(%a2)
        bra     st_good
4:      moveq   #0,%d0
        move.w  %d1,%d0
        addi.l  #FIRST_RES,%d0
        move.l  %d0,4(%a2)
        bsr     hw32
        movea.l S_REGS(%a3),%a0
        move.w  %d1,%d2
        add.w   %d2,%d2
        add.w   %d2,%d2
        addi.w  #R_RES,%d2
        move.l  0(%a0,%d2.w),%d0
        bsr     hw_back
        moveq   #0,%d2
        move.w  %d0,%d2
        move.l  %d2,12(%a2)             | csVerticalLines
        swap    %d0
        move.w  %d0,%d2
        move.l  %d2,8(%a2)              | csHorizontalPixels
        move.l  #60 << 16,16(%a2)       | csRefreshRate
        | The deepest mode that fits the VRAM.
        move.w  #FIRST_MODE+5,%d3
5:      bsr     depth_fits
        beq     6f
        subq.w  #1,%d3
        cmpi.w  #FIRST_MODE,%d3
        bhi     5b
6:      move.w  %d3,20(%a2)             | csMaxDepthMode
        clr.l   22(%a2)                 | csResolutionFlags
        clr.l   26(%a2)
        bra     st_good

| GetVideoParameters: csDisplayModeID (0 the current), csDepthMode in;
| the video parameter block at csVPBlockPtr, csPageCount, csDeviceType
| out.
st_getvideoparameters:
        move.l  (%a2),%d0
        bne     1f
        move.w  S_RES(%a3),%d1
        bra     2f
1:      subi.l  #FIRST_RES,%d0
        bmi     st_bad
        cmp.w   S_MODES(%a3),%d0
        bcc     st_bad
        move.w  %d0,%d1
2:      move.w  4(%a2),%d3
        bsr     depth_fits
        bne     st_bad
        move.l  6(%a2),%d0
        bsr     strip
        movea.l %d0,%a1                 | the VPBlock
        bsr     hw32
        movea.l S_REGS(%a3),%a0
        move.w  %d1,%d2
        add.w   %d2,%d2
        add.w   %d2,%d2
        addi.w  #R_RES,%d2
        move.l  0(%a0,%d2.w),%d5    | width << 16 | height
        bsr     hw_back
        move.l  %d5,%d0
        swap    %d0                     | width
        bsr     stride_of               | D0 = row bytes for D3
        clr.l   (%a1)+                  | vpBaseOffset
        move.w  %d0,(%a1)+              | vpRowBytes
        clr.l   (%a1)+                  | vpBounds: top, left
        move.w  %d5,(%a1)+              | bottom
        swap    %d5
        move.w  %d5,(%a1)+              | right
        clr.w   (%a1)+                  | vpVersion
        clr.w   (%a1)+                  | vpPackType
        clr.l   (%a1)+                  | vpPackSize
        move.l  #0x480000,(%a1)+        | vpHRes: 72 dpi
        move.l  #0x480000,(%a1)+        | vpVRes
        bsr     depth_of                | D0 = bits
        cmpi.w  #16,%d0
        bcs     3f
        move.w  #16,(%a1)+              | vpPixelType: ChunkyDirect
        move.w  %d0,(%a1)+              | vpPixelSize
        move.w  #3,(%a1)+               | vpCmpCount
        move.w  #5,%d2
        cmpi.w  #16,%d0
        beq     4f
        move.w  #8,%d2
4:      move.w  %d2,(%a1)+              | vpCmpSize
        moveq   #2,%d2
        bra     5f
3:      clr.w   (%a1)+                  | ChunkyIndexed
        move.w  %d0,(%a1)+
        move.w  #1,(%a1)+
        move.w  %d0,(%a1)+
        moveq   #0,%d2
5:      clr.l   (%a1)+                  | vpPlaneBytes
        move.l  #1,10(%a2)              | csPageCount
        move.l  %d2,14(%a2)             | csDeviceType: clutType or directType
        bra     st_good

| --- helpers ------------------------------------------------------------------

| D0 = the bits per pixel of depth mode D3.
depth_of:
        move.w  %d3,%d0
        subi.w  #FIRST_MODE,%d0
        move.w  %d1,-(%sp)
        move.w  %d0,%d1
        moveq   #1,%d0
        lsl.w   %d1,%d0
        move.w  (%sp)+,%d1
        rts

| D0 = the row bytes of width D0 (word) at depth mode D3: whole longwords.
stride_of:
        movem.l %d1-%d2,-(%sp)
        andi.l  #0xFFFF,%d0
        move.w  %d3,%d1
        subi.w  #FIRST_MODE,%d1
        lsl.l   %d1,%d0
        addi.l  #31,%d0
        lsr.l   #5,%d0
        lsl.l   #2,%d0
        movem.l (%sp)+,%d1-%d2
        rts

| Z set when depth mode D3 is valid and resolution D1 at that depth fits
| the VRAM.
depth_fits:
        movem.l %d0-%d2/%a0,-(%sp)
        cmpi.w  #FIRST_MODE,%d3
        bcs     8f
        cmpi.w  #FIRST_MODE+5,%d3
        bhi     8f
        bsr     hw32
        movea.l S_REGS(%a3),%a0
        add.w   %d1,%d1
        add.w   %d1,%d1
        addi.w  #R_RES,%d1
        move.l  0(%a0,%d1.w),%d2
        bsr     hw_back
        move.l  %d2,%d0
        swap    %d0
        bsr     stride_of
        mulu.w  %d2,%d0                 | row bytes x height
        cmp.l   S_VRAM(%a3),%d0
        bhi     8f
        movem.l (%sp)+,%d0-%d2/%a0
        ori.b   #4,%ccr                 | Z
        rts
8:      movem.l (%sp)+,%d0-%d2/%a0
        andi.b  #0xFB,%ccr              | not Z
        rts

| Depth mode D3 at the current resolution: the card's registers, the CLUT
| at 50% gray. Z set on success.
set_depth:
        movem.l %d1,-(%sp)
        move.w  S_RES(%a3),%d1
        bsr     depth_fits
        movem.l (%sp)+,%d1              | (MOVEM keeps the flags)
        bne     9f
        bsr     depth_of
        move.w  %d0,S_DEPTH(%a3)
        move.w  %d3,S_MODE(%a3)
        bclr    #F_DIRECT,S_FLAGS(%a3)
        cmpi.w  #16,%d0
        bcs     1f
        bset    #F_DIRECT,S_FLAGS(%a3)
1:      move.w  S_WIDTH(%a3),%d0
        bsr     stride_of
        move.w  %d0,S_STRIDE(%a3)
        bsr     hw32
        movea.l S_REGS(%a3),%a0
        move.l  %d0,R_STRIDE(%a0)
        move.l  #FB_BASE,R_BASE(%a0)
        moveq   #0,%d0
        move.w  S_DEPTH(%a3),%d0
        move.l  %d0,R_DEPTH(%a0)
        | Every CLUT entry 50% gray.
        clr.l   R_CLUT_INDEX(%a0)
        move.w  #3 * 256 - 1,%d1
        moveq   #0x7F,%d0
2:      move.l  %d0,R_CLUT_DATA(%a0)
        dbra    %d1,2b
        bsr     hw_back
        cmp.w   %d0,%d0
9:      rts

| SetEntries and DirectSetEntries: csTable, csStart (-1: by the value
| fields), csCount (zero based). Indexed depths reach their 2^depth entries.
| D0 = the result.
set_entries:
        move.l  (%a2),%d0
        bsr     strip
        movea.l %d0,%a1                 | csTable
        move.w  4(%a2),%d4              | csStart
        move.w  6(%a2),%d5              | csCount
        bmi     9f
        move.w  #256,%d6                | entries reachable
        btst    #F_DIRECT,S_FLAGS(%a3)
        bne     1f
        move.w  S_DEPTH(%a3),%d0
        moveq   #1,%d6
        lsl.w   %d0,%d6
1:      tst.w   %d4
        bmi     2f
        move.w  %d4,%d0
        add.w   %d5,%d0
        cmp.w   %d6,%d0
        bcc     9f
2:      bsr     hw32
        movea.l S_REGS(%a3),%a0
3:      move.w  %d4,%d0
        bpl     4f
        move.w  (%a1),%d0               | value
        cmp.w   %d6,%d0
        bcc     8f
        bra     5f
4:      addq.w  #1,%d4
5:      andi.l  #0xFF,%d0
        move.l  %d0,R_CLUT_INDEX(%a0)
        moveq   #0,%d1
        moveq   #0,%d2
        moveq   #0,%d3
        move.b  2(%a1),%d1              | red, green, blue: the high bytes
        move.b  4(%a1),%d2
        move.b  6(%a1),%d3
        btst    #F_GRAY,S_FLAGS(%a3)
        beq     6f
        btst    #F_DIRECT,S_FLAGS(%a3)
        bne     6f
        | Luminance: 0.30 R + 0.59 G + 0.11 B (in 256ths).
        mulu.w  #77,%d1
        mulu.w  #151,%d2
        mulu.w  #28,%d3
        add.l   %d2,%d1
        add.l   %d3,%d1
        lsr.l   #8,%d1
        move.l  %d1,%d2
        move.l  %d1,%d3
6:      move.l  %d1,R_CLUT_DATA(%a0)
        move.l  %d2,R_CLUT_DATA(%a0)
        move.l  %d3,R_CLUT_DATA(%a0)
        addq.l  #8,%a1
        dbra    %d5,3b
        bsr     hw_back
        moveq   #0,%d0
        rts
8:      bsr     hw_back
9:      move.w  #controlErr,%d0
        rts

| The current page filled with the depth's 50% dither (32-bit mode): rows
| alternate between the depth's two patterns, each a pair of longwords.
gray_fill:
        .ifdef  CARD24
        move.l  S_BASE(%a3),%d0
        andi.l  #0xFF0FFFFF,%d0         | the VRAM at $Fs00 0040
        movea.l %d0,%a0
        .else
        movea.l S_BASE(%a3),%a0
        .endif
        move.w  S_MODE(%a3),%d0
        subi.w  #FIRST_MODE,%d0
        lsl.w   #4,%d0
        lea     dither(%pc),%a1
        adda.w  %d0,%a1
        move.w  S_HEIGHT(%a3),%d2
        subq.w  #1,%d2
        moveq   #0,%d3
        move.w  S_STRIDE(%a3),%d3
        lsr.w   #2,%d3
        subq.w  #1,%d3                  | longwords per row - 1
        moveq   #0,%d5                  | 0 on even rows, 8 on odd ones
1:      move.l  0(%a1,%d5.w),%d0
        move.l  4(%a1,%d5.w),%d1
        move.w  %d3,%d4
2:      move.l  %d0,(%a0)+
        exg     %d0,%d1
        dbra    %d4,2b
        eori.w  #8,%d5
        dbra    %d2,1b
        rts

| Per depth: the even row's two longwords, the odd row's.
dither:
        .long   0xAAAAAAAA, 0xAAAAAAAA, 0x55555555, 0x55555555     | 1 bit: black, white
        .long   0xCCCCCCCC, 0xCCCCCCCC, 0x33333333, 0x33333333     | 2
        .long   0xF0F0F0F0, 0xF0F0F0F0, 0x0F0F0F0F, 0x0F0F0F0F     | 4
        .long   0xFF00FF00, 0xFF00FF00, 0x00FF00FF, 0x00FF00FF     | 8
        .long   0x00007FFF, 0x00007FFF, 0x7FFF0000, 0x7FFF0000     | 16: black, white
        .long   0x00000000, 0x00FFFFFF, 0x00FFFFFF, 0x00000000     | 32

| A linear ramp in each channel of the CLUT (direct depths, 32-bit mode).
clut_ramp:
        movea.l S_REGS(%a3),%a0
        clr.l   R_CLUT_INDEX(%a0)
        moveq   #0,%d0
        move.w  #255,%d1
1:      move.l  %d0,R_CLUT_DATA(%a0)
        move.l  %d0,R_CLUT_DATA(%a0)
        move.l  %d0,R_CLUT_DATA(%a0)
        addq.w  #1,%d0
        dbra    %d1,1b
        rts

| The slot's PRAM record into S_PRAM; Z set when read.
pram_read:
        bsr     spb_clear
        lea     S_PRAM(%a3),%a1
        move.l  %a1,spResult(%a0)
        moveq   #sReadPRAMRec,%d0
        .short  _SlotManager
        tst.w   %d0
        rts

| S_PRAM back into the slot's PRAM; Z set when written.
pram_write:
        bsr     spb_clear
        lea     S_PRAM(%a3),%a1
        move.l  %a1,spsPointer(%a0)
        moveq   #sPutPRAMRec,%d0
        .short  _SlotManager
        tst.w   %d0
        rts

| Video sResource 128 + D0 enabled (D2 = 0) or disabled (D2 = 1):
| SetSRsrcState (Devices 2-51). Keeps D1.
srsrc_state:
        movem.l %d1/%a1,-(%sp)
        move.w  %d0,%d1
        bsr     spb_clear
        addi.b  #FIRST_RES,%d1
        move.b  %d1,spID(%a0)
        move.l  %d2,spParamData(%a0)
        moveq   #SetSRsrcState,%d0
        .short  _SlotManager
        movem.l (%sp)+,%d1/%a1
        rts

| A0 = the storage's Slot Manager parameter block, cleared, for this slot.
spb_clear:
        lea     S_SPB(%a3),%a0
        moveq   #spBlockSize / 2 - 1,%d0
1:      clr.w   (%a0)+
        dbra    %d0,1b
        lea     S_SPB(%a3),%a0
        move.b  S_SLOT(%a3),spSlot(%a0)
        rts
