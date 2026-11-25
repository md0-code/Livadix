| Livadix — the PrimaryInit code of the Livadix Display Card's declaration ROM
| (the card's registers in src/hw/livadix_card.h).
|
| The Slot Manager runs it at startup with A0 = the slot execution
| parameter block (Inside Macintosh: Devices, chapter 2, "SEBlock"). The
| card has one video sResource per resolution (128 + n, in the order of its
| RES table); the display it drives was set up for one of them, which the
| card reports in its MONITOR register, as a real card learns its
| monitor's sense code. PrimaryInit puts the card in its start state (1
| bit, display on, VBL interrupts off) and disables every other video
| sResource with SetSRsrcState, so the system starts on one display mode
| family member and finds the others (SGetSRsrc sees disabled sResources)
| when it switches resolution, as Designing Cards and Drivers for the
| Macintosh Family, 3rd ed., chapter 8 "PrimaryInit" and "SecondaryInit"
| describe for cards with video mode families. The resolution chosen in
| the system (SetDefaultMode or SavePreferredConfiguration, livadixvideo.s:
| vendorUse2 of the slot's PRAM, its video sResource) is kept across
| restarts while the display stays the same: vendorUse3 holds the MONITOR
| value + 1 the record belongs to, and a different display (another
| video.resolution) forgets the choice, as a real card forgets the modes
| of a monitor it no longer senses. A chosen resolution the system's depth
| (vendorUse1) does not fit in the VRAM is not taken. Assembled with CARD24
| (livadixcard_init24.s, for the Macintosh II family's 24-bit card ROM): where
| SVersion fails (the original Slot Manager, which has no SetSRsrcState;
| chapter 8, "PrimaryInit": "Call _sVersion ... If the call returns an
| error, then the old Slot Manager is in ROM"), the other sResources are
| deleted with SDeleteSRTRec instead. seStatus 0: initialised.
|
| Besides the Slot Manager (chapter 8, "PrimaryInit"; SReadPRAMRec $11 and
| SPutPRAMRec $12, Devices chapter 2), only _SwapMMUMode is
| called: the card's registers are at its 32-bit address, and the Slot
| Manager runs PrimaryInit in 24-bit mode on the Quadra 700 (chapter 1:
| "it is usually still necessary to call SwapMMUMode to ensure that the
| machine is in 32-bit mode").
|
| Sources: Designing Cards and Drivers, 3rd ed., chapter 8; Inside
| Macintosh: Devices (1994), chapter 2 (SEBlock and SpBlock layouts,
| _SlotManager selectors $08 SVersion, smSelOOBErr before System 7, and $09
| SetSRsrcState); Inside Macintosh V, "The Slot Manager" (selector 49
| SDeleteSRTRec: spSlot, spID, spExtDev).
|
| Assembler: GNU as for m68k (binutils), -m68020; built by
| tools/guest_build.py into src/hw/guest/bin/livadixcard_init.bin.

        .set    _SlotManager, 0xA06E
        .set    _SwapMMUMode, 0xA05D
        .set    SVersion, 0x08
        .set    SetSRsrcState, 0x09
        .set    SDeleteSRTRec, 49
        .set    SReadPRAMRec, 0x11
        .set    SPutPRAMRec, 0x12
        .set    spResult, 0
        .set    spsPointer, 4
        .set    spParamData, 24
        .set    seSlot, 0
        .set    seStatus, 2
        .set    spSlot, 49
        .set    spID, 50
        .set    spExtDev, 51
        .set    spBlockSize, 56
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
        .set    R_RES, 0x100
        .set    FIRST_RES, 128

        .text
start:
        movem.l %d3-%d7/%a2-%a4,-(%sp)
        movea.l %a0,%a2                 | the SEBlock
        moveq   #0,%d7
        move.b  seSlot(%a2),%d7
        | A Slot Manager parameter block and the slot's PRAM record after it.
        lea     -spBlockSize-8(%sp),%sp
        movea.l %sp,%a4
        bsr     spb_clear
        lea     spBlockSize(%a4),%a1
        clr.l   (%a1)
        clr.l   4(%a1)
        move.l  %a1,spResult(%a4)
        movea.l %a4,%a0
        moveq   #SReadPRAMRec,%d0
        .short  _SlotManager
        move.l  %d7,%d0
        ror.l   #8,%d0
        ori.l   #0xF0000000,%d0         | $Fs00 0000
        addi.l  #REGS,%d0
        movea.l %d0,%a3
        moveq   #1,%d0
        .short  _SwapMMUMode
        move.b  %d0,-(%sp)              | the mode to return to
        | The resolution: the display's, or the one chosen for it.
        lea     spBlockSize(%a4),%a1    | the PRAM record
        move.l  R_MONITOR(%a3),%d6
        move.l  R_MODES(%a3),%d5
        moveq   #0,%d2                  | the record unchanged
        moveq   #0,%d0
        move.b  4(%a1),%d0              | vendorUse3: the display + 1
        move.l  %d6,%d1
        addq.l  #1,%d1
        cmp.l   %d1,%d0
        beq     1f
        clr.b   3(%a1)                  | another display: no choice
        move.b  %d1,4(%a1)
        moveq   #1,%d2
        bra     3f
1:      moveq   #0,%d1
        move.b  3(%a1),%d1              | vendorUse2: the chosen sResource
        subi.l  #FIRST_RES,%d1
        bmi     3f
        cmp.l   %d5,%d1
        bge     3f
        moveq   #0,%d0
        move.b  2(%a1),%d0              | vendorUse1: the depth mode
        subi.l  #FIRST_RES,%d0
        bcs     2f
        cmpi.l  #5,%d0
        bhi     2f
        move.l  %d1,%d3
        lsl.l   #2,%d3
        move.l  R_RES(%a3,%d3.l),%d3    | width << 16 | height
        moveq   #0,%d4
        move.w  %d3,%d4
        clr.w   %d3
        swap    %d3
        lsl.l   %d0,%d3                 | width x bits
        addi.l  #31,%d3
        lsr.l   #5,%d3
        lsl.l   #2,%d3                  | row bytes
        mulu.w  %d4,%d3
        cmp.l   R_VRAM(%a3),%d3
        bhi     3f
2:      move.l  %d1,%d6
3:
        | The start state: that size in 1 bit, VBL interrupts off.
        move.l  #1,R_CONTROL(%a3)
        move.l  #1,R_STATUS(%a3)
        move.l  %d6,%d0
        lsl.l   #2,%d0
        move.l  R_RES(%a3,%d0.l),%d0
        moveq   #0,%d1
        move.w  %d0,%d1
        move.l  %d1,R_HEIGHT(%a3)
        swap    %d0
        move.w  %d0,%d1
        move.l  %d1,R_WIDTH(%a3)
        addi.l  #31,%d1
        lsr.l   #5,%d1
        lsl.l   #2,%d1
        move.l  %d1,R_STRIDE(%a3)
        move.l  #1,R_DEPTH(%a3)
        move.l  #FB_BASE,R_BASE(%a3)
        move.b  (%sp)+,%d0
        .short  _SwapMMUMode
        | The record kept for this display.
        tst.l   %d2
        beq     4f
        bsr     spb_clear
        lea     spBlockSize(%a4),%a1
        move.l  %a1,spsPointer(%a4)
        movea.l %a4,%a0
        moveq   #SPutPRAMRec,%d0
        .short  _SlotManager
4:
        | Every other resolution's video sResource disabled.
        .ifdef  CARD24
        moveq   #SetSRsrcState,%d3
        movea.l %a4,%a0                 | the original Slot Manager:
        moveq   #SVersion,%d0           | SDeleteSRTRec instead
        .short  _SlotManager
        tst.w   %d0
        beq     5f
        moveq   #SDeleteSRTRec,%d3
5:
        .endif
        moveq   #0,%d4
1:      cmp.l   %d5,%d4
        bge     3f
        cmp.l   %d6,%d4
        beq     2f
        movea.l %a4,%a0
        moveq   #spBlockSize / 2 - 1,%d0
4:      clr.w   (%a0)+
        dbra    %d0,4b
        move.b  %d7,spSlot(%a4)
        move.b  %d4,%d0
        addi.b  #FIRST_RES,%d0
        move.b  %d0,spID(%a4)
        clr.b   spExtDev(%a4)
        move.l  #1,spParamData(%a4)     | disabled
        movea.l %a4,%a0
        .ifdef  CARD24
        move.l  %d3,%d0
        .else
        moveq   #SetSRsrcState,%d0
        .endif
        .short  _SlotManager
2:      addq.l  #1,%d4
        bra     1b
3:      lea     spBlockSize+8(%sp),%sp
        clr.w   seStatus(%a2)
        movem.l (%sp)+,%d3-%d7/%a2-%a4
        rts

| A4 = the parameter block, cleared, for the slot D7. Keeps A1.
spb_clear:
        movea.l %a4,%a0
        moveq   #spBlockSize / 2 - 1,%d0
5:      clr.w   (%a0)+
        dbra    %d0,5b
        move.b  %d7,spSlot(%a4)
        rts
