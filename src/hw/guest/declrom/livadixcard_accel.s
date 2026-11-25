| Livadix — the SecondaryInit code of the Livadix Display Card's declaration ROM
| (the card's registers in
| src/hw/livadix_card.h): the card's accelerated QuickDraw.
|
| The Slot Manager runs SecondaryInit once the System file's patches are in
| (Designing Cards and Drivers for the Macintosh Family, 3rd ed., chapter 8,
| "SecondaryInit"), with A0 = the slot execution parameter block. When the
| card has its blitter (BLT_CAPS bit 0: video.acceleration) and the
| Macintosh runs in 32-bit mode (low memory MMU32Bit $CB2: the card's
| registers and VRAM at their 32-bit addresses), it copies the patch below
| into the system heap (_NewPtr with SYS, _BlockMove, which also flushes
| the instruction cache) and puts it in front of two of QuickDraw's
| bottlenecks (Inside Macintosh I, "QuickDraw", "Customizing QuickDraw
| Operations"; the trap table with _GetToolTrapAddress $A746 and
| _SetToolTrapAddress $A647, Inside Macintosh: Operating System
| Utilities, "Trap Manager"):
|   StdRect ($A8A0): paint, erase, fill and invert of a rectangle;
|   StdBits ($A8EB): CopyBits' transfer.
| A call is the blitter's when the current port is a color port drawing
| into the card's screen, the card's being the only screen, and when the
| call is of a kind it draws exactly as QuickDraw would; anything else goes
| on to the routine that was there before (QuickDraw, or a patch of the
| System's). The blitter's calls:
|   paint (pnMode patCopy), erase and fill with an old-style
|     pattern (a PixPat of patType 0; its 1 bits in the port's fgColor, its
|     0 bits in bkColor, Inside Macintosh V, "Color QuickDraw"; traced on
|     the Quadra 700 ROM: Color QuickDraw keeps such a pattern's eight bytes
|     in the PixPat's patData, pat1Data staying as NewPixPat left it);
|   invert, unless the hilite mode is set (HiliteMode $938 bit 7 clear,
|     Inside Macintosh V, "Highlighting"): every bit of an indexed pixel,
|     the colour bits of a direct one (checked against QuickDraw: 16 bits
|     keep their unused high bit, 32 bits their high byte);
|   CopyBits from the card's screen to itself: srcCopy, no mask region, the
|     same size (no stretching), the same depth and colour table, a black
|     foreground and white background (no colourizing), the source rectangle
|     inside the source's bounds (QuickDraw reads whatever memory lies
|     outside them);
| none smaller than 1 KB of pixels (there setting the blitter up over
| NuBus and hiding the cursor take about as long as QuickDraw's drawing),
| and
| none while the pen is hidden (pnVis negative: QuickDraw then draws no
| rectangle at all, as while a picture is defined), a picture, region or
| polygon is being recorded (picSave, rgnSave, polySave) or patStretch is
| set. The clip is the port's visRgn
| and clipRgn (one of them may be any region: its scan lines are walked
| into the card's clip list, at most 128 rectangles; Inside Macintosh I,
| "QuickDraw", the region structure: rgnSize, rgnBBox, then per changing
| scan line its v and the h inversion points, $7FFF ending each list and
| the region) and the pixel map's bounds. The cursor is hidden over the
| pixels the blitter changes (_ShieldCursor with the global rectangle,
| _ShowCursor after), as QuickDraw hides it while it draws on a screen.
| GrafPort and CGrafPort fields from Inside Macintosh I and V (portBits or
| portPixMap 2, portVersion 6, portRect 16, visRgn 24, clipRgn 28, bkPixPat
| 32, rgbFgColor 36, rgbBkColor 42, pnMode 56, pnPixPat 58, fillPixPat 62,
| pnVis 66, fgColor 80, bkColor 84, patStretch 90, picSave 92, rgnSave 96,
| polySave 100); PixMap (baseAddr 0, rowBytes 4 with its flag bits $8000,
| bounds 6, pixelSize 32, pmTable 42), PixPat (patType 0, patData 6),
| GDevice (gdPMap 22, gdNextGD 30), ColorTable (ctSeed 0) and low memory
| DeviceList ($8A8) from Inside Macintosh V. Pattern alignment: a pattern
| is aligned to the port's local coordinates (the pattern's row v mod 8
| and column h mod 8 of the point (h, v)), checked pixel for pixel against
| QuickDraw (tests/verify.py, check accel-equal).
|
| On a Power Macintosh (Gestalt 'sysa', gestaltSysArchitecture, answering
| gestaltPowerPC 2 in A0: Inside Macintosh: Operating System Utilities,
| "Gestalt Manager"; Inside Macintosh: PowerPC System Software, chapter 1)
| SecondaryInit installs the native patch instead
| (src/hw/guest/pci/livadixcard_accel_ppc.s, the code fragment carried below):
| a copy of a routine descriptor and the fragment's PEF container in the
| system heap (_BlockMove), the fragment prepared by the Code Fragment
| Manager (GetMemFragment, _CodeFragmentDispatch $AA5A selector 3 with
| kPrivateCFragCopy, its Pascal interface from CodeFragments.h; Inside
| Macintosh: PowerPC System Software, chapter 3), and its main symbol,
| install(vram, registers), called through the routine descriptor
| (_MixedModeMagic $AAFE, version 7; the routine record kPowerPCISA with
| kUseNativeISA, ProcInfo kCStackBased with a 4-byte result and two 4-byte
| arguments, $3F1; Inside Macintosh: PowerPC System Software, chapter 2).
| If the fragment cannot be prepared, the 68k patch is installed.
|
| Assembler: GNU as for m68k (binutils), -m68020; built by
| tools/guest_build.py into src/hw/guest/bin/livadixcard_accel.bin, after the
| fragment's container (src/hw/guest/bin/livadixcard_accel_pef.bin).

        .set    _NewPtrSys, 0xA51E
        .set    _BlockMove, 0xA02E
        .set    _GetToolTrap, 0xA746
        .set    _SetToolTrap, 0xA647
        .set    _ShieldCursor, 0xA855
        .set    _ShowCursor, 0xA853
        .set    _Gestalt, 0xA1AD
        .set    _CodeFragmentDispatch, 0xAA5A
        .set    gestaltSysArchitecture, 0x73797361      | 'sysa'
        .set    gestaltPowerPC, 2
        .set    kPrivateCFragCopy, 5
        .set    selGetMemFragment, 3
        .set    RD_SIZE, 32
        .set    RD_PROC, 20                             | the routine record's procedure
        .set    MMU32Bit, 0x0CB2
        .set    DeviceList, 0x08A8
        .set    HiliteMode, 0x0938
        .set    seSlot, 0
        .set    seStatus, 2
        .set    REGS, 0xC00000
        .set    R_DEPTH, 0x028
        .set    R_STRIDE, 0x02C
        .set    R_BASE, 0x030
        .set    BLT_CAPS, 0x200
        .set    BLT_STATUS, 0x204
        .set    BLT_OP, 0x208
        .set    BLT_DEPTH, 0x20C
        .set    BLT_DST, 0x210
        .set    BLT_DST_STRIDE, 0x214
        .set    BLT_SRC, 0x218
        .set    BLT_SRC_STRIDE, 0x21C
        .set    BLT_DX, 0x220
        .set    BLT_DY, 0x224
        .set    BLT_W, 0x228
        .set    BLT_H, 0x22C
        .set    BLT_SX, 0x230
        .set    BLT_SY, 0x234
        .set    BLT_FG, 0x238
        .set    BLT_BG, 0x23C
        .set    BLT_PAT0, 0x240
        .set    BLT_PAT1, 0x244
        .set    BLT_PATX, 0x248
        .set    BLT_PATY, 0x24C
        .set    BLT_GO, 0x250
        .set    BLT_CLIPS, 0x258
        .set    CLIP, 0x400
        .set    MAX_CLIPS, 128
        .set    MIN_BYTES, 1024
        .set    OP_FILL, 1
        .set    OP_INVERT, 2
        .set    OP_COPY, 3
        .set    TRAP_STDRECT, 0xA0
        .set    TRAP_STDBITS, 0xEB

        .text
start:
        movem.l %d3-%d7/%a2-%a4,-(%sp)
        clr.w   seStatus(%a0)
        tst.b   MMU32Bit.w
        beq     9f
        moveq   #0,%d7
        move.b  seSlot(%a0),%d7
        ror.l   #8,%d7
        ori.l   #0xF0000000,%d7         | $Fs00 0000: the VRAM
        movea.l %d7,%a3
        lea     REGS(%a3),%a4
        move.l  BLT_CAPS(%a4),%d0
        btst    #0,%d0
        beq     9f
        move.l  #gestaltSysArchitecture,%d0
        .short  _Gestalt
        tst.w   %d0
        bne     1f
        moveq   #gestaltPowerPC,%d0
        cmp.l   %a0,%d0
        bne     1f
        bsr     native
        tst.l   %d0
        beq     9f
1:      move.l  #patch_end-patch,%d0
        .short  _NewPtrSys
        move.l  %a0,%d0
        beq     9f
        movea.l %a0,%a2
        movea.l %a0,%a1
        lea     patch(%pc),%a0
        move.l  #patch_end-patch,%d0
        .short  _BlockMove
        move.l  %a3,v_vram-patch(%a2)
        move.l  %a4,v_regs-patch(%a2)
        move.w  #TRAP_STDRECT,%d0
        .short  _GetToolTrap
        move.l  %a0,v_rect-patch(%a2)
        move.w  #TRAP_STDBITS,%d0
        .short  _GetToolTrap
        move.l  %a0,v_bits-patch(%a2)
        lea     rect_patch-patch(%a2),%a0
        move.w  #TRAP_STDRECT,%d0
        .short  _SetToolTrap
        lea     bits_patch-patch(%a2),%a0
        move.w  #TRAP_STDBITS,%d0
        .short  _SetToolTrap
9:      movem.l (%sp)+,%d3-%d7/%a2-%a4
        rts

| The native patch on a Power Macintosh: A3 the VRAM, A4 the registers.
| Out: D0 0 when it is installed.
        .set    N_CONN, -4              | the connection
        .set    N_MAIN, -8              | the main symbol
        .set    N_ERR, -8-256           | the error's name (Str255)
native:
        link    %a6,#N_ERR
        move.l  #ppc_end-ppc_rd,%d0
        .short  _NewPtrSys
        move.l  %a0,%d0
        beq     8f
        movea.l %a0,%a2
        movea.l %a0,%a1
        lea     ppc_rd(%pc),%a0
        move.l  #ppc_end-ppc_rd,%d0
        .short  _BlockMove
        subq.l  #2,%sp                  | GetMemFragment's OSErr
        pea     RD_SIZE(%a2)            | memAddr: the container
        move.l  #ppc_end-ppc_pef,-(%sp) | length
        pea     ppc_name(%pc)           | fragName
        pea     kPrivateCFragCopy       | findFlags
        pea     N_CONN(%a6)             | connID
        pea     N_MAIN(%a6)             | mainAddr
        pea     N_ERR(%a6)              | errMessage
        move.w  #selGetMemFragment,-(%sp)
        .short  _CodeFragmentDispatch
        move.w  (%sp)+,%d0
        bne     8f
        move.l  N_MAIN(%a6),%d0
        beq     8f
        move.l  %d0,RD_PROC(%a2)
        move.l  %a4,-(%sp)              | install(vram, registers)
        move.l  %a3,-(%sp)
        jsr     (%a2)
        addq.l  #8,%sp
        unlk    %a6
        rts
8:      moveq   #-1,%d0
        unlk    %a6
        rts

| --- The patch, copied into the system heap -------------------------------

patch:
v_rect: .long   0                       | StdRect before the patch
v_bits: .long   0                       | StdBits before the patch
v_vram: .long   0                       | the card's VRAM, $Fs00 0000
v_regs: .long   0                       | its registers

| StdRect(verb: GrafVerb; r: Rect): 4(sp) r, 8(sp) the verb (high byte).
rect_patch:
        movem.l %d3-%d7/%a2-%a4,-(%sp)
        bsr     check
        beq     r_fall
        tst.w   66(%a2)                 | pnVis: a hidden pen draws nothing
        bmi     r_fall
        moveq   #0,%d3
        move.b  40(%sp),%d3             | the verb
        cmpi.b  #3,%d3
        beq     r_invert
        cmpi.b  #1,%d3
        beq     r_paint
        cmpi.b  #2,%d3
        beq     r_erase
        cmpi.b  #4,%d3
        bne     r_fall
        movea.l 62(%a2),%a0             | fill: fillPixPat
        bra     r_pat
r_paint:
        cmpi.w  #8,56(%a2)              | pnMode patCopy
        bne     r_fall
        movea.l 58(%a2),%a0             | pnPixPat
        bra     r_pat
r_erase:
        movea.l 32(%a2),%a0             | bkPixPat
r_pat:
        movea.l (%a0),%a0
        tst.w   (%a0)                   | patType 0: an old-style pattern
        bne     r_fall
        move.l  6(%a0),%d0              | patData: its eight bytes
        beq     r_fall
        movea.l %d0,%a0
        move.l  (%a0),%d0
        beq     r_fall
        movea.l %d0,%a0
        move.l  (%a0),BLT_PAT0(%a4)
        move.l  4(%a0),BLT_PAT1(%a4)
        move.l  80(%a2),%d0
        move.l  84(%a2),%d1
        cmpi.w  #2,32(%a3)              | at 1 and 2 bits, two distinct RGB
        bhi     1f                      | colours that map to one index are
        cmp.l   %d0,%d1                 | separated by QuickDraw (patExpand)
        bne     1f
        move.l  36(%a2),%d2
        cmp.l   42(%a2),%d2
        bne     r_fall
        move.w  40(%a2),%d2
        cmp.w   46(%a2),%d2
        bne     r_fall
1:      move.l  %d0,BLT_FG(%a4)
        move.l  %d1,BLT_BG(%a4)
        moveq   #OP_FILL,%d3
        bra     r_draw
r_invert:
        btst    #7,HiliteMode.w
        beq     r_fall                  | the hilite mode: QuickDraw's
        moveq   #-1,%d0
        move.l  %d0,BLT_PAT0(%a4)
        move.l  %d0,BLT_PAT1(%a4)
        | Every bit of an index; a direct pixel's colour bits only.
        move.w  32(%a3),%d1
        cmpi.w  #16,%d1
        bne     1f
        move.l  #0x7FFF,%d0
1:      cmpi.w  #32,%d1
        bne     2f
        move.l  #0xFFFFFF,%d0
2:      move.l  %d0,BLT_FG(%a4)
        moveq   #OP_INVERT,%d3
r_draw:
        movea.l 36(%sp),%a0             | the rectangle
        movem.w (%a0),%d4-%d7
        bsr     clip
        bmi     r_fall
        beq     r_done
        bsr     small
        bcs     r_fall
        bsr     dst_regs
        move.l  %d3,BLT_OP(%a4)
        bsr     shield
        move.l  %d0,BLT_GO(%a4)
        bsr     wait
        .short  _ShowCursor
r_done:
        movem.l (%sp)+,%d3-%d7/%a2-%a4
        movea.l (%sp)+,%a0
        addq.l  #6,%sp
        jmp     (%a0)
r_fall:
        movem.l (%sp)+,%d3-%d7/%a2-%a4
        move.l  v_rect(%pc),-(%sp)
        rts

| StdBits(VAR srcBits: BitMap; VAR srcRect, dstRect: Rect; mode: INTEGER;
| maskRgn: RgnHandle): 8(a6) maskRgn, 12 mode, 14 dstRect, 18 srcRect, 22
| srcBits. Locals: -4 the source pixel map, -8 and -12 the source's pixel
| offset from the destination (v, h).
bits_patch:
        link    %a6,#-12
        movem.l %d3-%d7/%a2-%a4,-(%sp)
        bsr     check
        beq     b_fall
        tst.l   8(%a6)                  | no mask region
        bne     b_fall
        tst.w   12(%a6)                 | srcCopy
        bne     b_fall
        tst.l   36(%a2)                 | rgbFgColor black
        bne     b_fall
        tst.w   40(%a2)
        bne     b_fall
        moveq   #-1,%d0
        cmp.l   42(%a2),%d0             | rgbBkColor white
        bne     b_fall
        cmp.w   46(%a2),%d0
        bne     b_fall
        movea.l 22(%a6),%a0             | srcBits
        move.w  4(%a0),%d0
        bpl     b_fall                  | a BitMap
        andi.w  #0xC000,%d0
        cmpi.w  #0xC000,%d0
        bne     1f
        movea.l (%a0),%a0               | a color port's portPixMap
        movea.l (%a0),%a0
1:      move.l  (%a0),%d0
        cmp.l   (%a3),%d0               | the same screen
        bne     b_fall
        move.w  4(%a0),%d0
        andi.w  #0x3FFF,%d0
        move.w  4(%a3),%d1
        andi.w  #0x3FFF,%d1
        cmp.w   %d1,%d0
        bne     b_fall
        move.w  32(%a0),%d0
        cmp.w   32(%a3),%d0
        bne     b_fall
        cmpi.w  #8,%d0
        bhi     2f
        move.l  42(%a0),%d0             | indexed: the same colour table
        beq     b_fall
        movea.l %d0,%a1
        move.l  (%a1),%d0
        beq     b_fall
        movea.l %d0,%a1
        move.l  (%a1),%d1               | its ctSeed
        move.l  42(%a3),%d0
        beq     b_fall
        movea.l %d0,%a1
        move.l  (%a1),%d0
        beq     b_fall
        movea.l %d0,%a1
        cmp.l   (%a1),%d1
        bne     b_fall
2:      move.l  %a0,-4(%a6)
        movea.l 14(%a6),%a0             | dstRect
        movea.l 18(%a6),%a1             | srcRect
        move.w  4(%a0),%d0
        sub.w   (%a0),%d0
        move.w  4(%a1),%d1
        sub.w   (%a1),%d1
        cmp.w   %d0,%d1
        bne     b_fall
        move.w  6(%a0),%d0
        sub.w   2(%a0),%d0
        move.w  6(%a1),%d1
        sub.w   2(%a1),%d1
        cmp.w   %d0,%d1
        bne     b_fall
        movem.w (%a0),%d4-%d7           | the target: dstRect
        | QuickDraw reads a source outside its pixel map's bounds as the
        | memory there: only a source inside them is the blitter's.
        movea.l -4(%a6),%a0
        move.w  (%a1),%d0
        cmp.w   6(%a0),%d0
        blt     b_fall
        move.w  2(%a1),%d0
        cmp.w   8(%a0),%d0
        blt     b_fall
        move.w  4(%a1),%d0
        cmp.w   10(%a0),%d0
        bgt     b_fall
        move.w  6(%a1),%d0
        cmp.w   12(%a0),%d0
        bgt     b_fall
        movea.l %a0,%a1
        | The source pixel of a destination pixel: (srcRect.topLeft -
        | srcBounds.topLeft) - (dstRect.topLeft - dstBounds.topLeft) on.
        movea.l 18(%a6),%a0
        move.w  (%a0),%d0
        sub.w   6(%a1),%d0
        move.w  2(%a0),%d1
        sub.w   8(%a1),%d1
        movea.l 14(%a6),%a0
        sub.w   (%a0),%d0
        add.w   6(%a3),%d0
        sub.w   2(%a0),%d1
        add.w   8(%a3),%d1
        ext.l   %d0
        ext.l   %d1
        move.l  %d0,-8(%a6)
        move.l  %d1,-12(%a6)
        bsr     clip_target
        bmi     b_fall
        beq     b_done
        bsr     small
        bcs     b_fall
        moveq   #OP_COPY,%d3
        bsr     dst_regs
        move.l  %d3,BLT_OP(%a4)
        movea.l -4(%a6),%a0
        move.l  (%a0),%d0
        sub.l   v_vram(%pc),%d0
        move.l  %d0,BLT_SRC(%a4)
        moveq   #0,%d0
        move.w  4(%a0),%d0
        andi.w  #0x3FFF,%d0
        move.l  %d0,BLT_SRC_STRIDE(%a4)
        move.l  %d5,%d0
        add.l   -12(%a6),%d0
        move.l  %d0,BLT_SX(%a4)
        move.l  %d4,%d0
        add.l   -8(%a6),%d0
        move.l  %d0,BLT_SY(%a4)
        | The cursor off both rectangles: their union.
        movem.l %d4-%d7,-(%sp)
        move.l  -8(%a6),%d0
        bpl     1f
        add.l   %d0,%d4
        bra     2f
1:      add.l   %d0,%d6
2:      move.l  -12(%a6),%d0
        bpl     3f
        add.l   %d0,%d5
        bra     4f
3:      add.l   %d0,%d7
4:      bsr     shield
        movem.l (%sp)+,%d4-%d7
        move.l  %d0,BLT_GO(%a4)
        bsr     wait
        .short  _ShowCursor
b_done:
        movem.l (%sp)+,%d3-%d7/%a2-%a4
        unlk    %a6
        movea.l (%sp)+,%a0
        lea     18(%sp),%sp
        jmp     (%a0)
b_fall:
        movem.l (%sp)+,%d3-%d7/%a2-%a4
        unlk    %a6
        move.l  v_bits(%pc),-(%sp)
        rts

| Whether thePort draws into the card's screen, the only screen, as a
| color port recording nothing. Out: D0 0 (Z) when not; else A2 the port,
| A3 its pixel map, A4 the card's registers.
check:
        movea.l (%a5),%a0
        movea.l (%a0),%a2               | thePort
        move.w  6(%a2),%d0
        andi.w  #0xC000,%d0
        cmpi.w  #0xC000,%d0
        bne     1f
        tst.l   92(%a2)                 | picSave
        bne     1f
        tst.l   96(%a2)                 | rgnSave
        bne     1f
        tst.l   100(%a2)                | polySave
        bne     1f
        tst.w   90(%a2)                 | patStretch
        bne     1f
        movea.l 2(%a2),%a3
        movea.l (%a3),%a3               | portPixMap
        movea.l v_regs(%pc),%a4
        move.l  (%a3),%d0
        sub.l   v_vram(%pc),%d0
        cmp.l   R_BASE(%a4),%d0
        bne     1f
        moveq   #0,%d0
        move.w  4(%a3),%d0
        bpl     1f
        andi.w  #0x3FFF,%d0
        cmp.l   R_STRIDE(%a4),%d0
        bne     1f
        moveq   #0,%d0
        move.w  32(%a3),%d0
        cmp.l   R_DEPTH(%a4),%d0
        bne     1f
        movea.l DeviceList.w,%a0
        movea.l (%a0),%a0
        tst.l   30(%a0)                 | gdNextGD: the only screen
        bne     1f
        movea.l 22(%a0),%a0
        movea.l (%a0),%a0               | gdPMap
        move.l  (%a0),%d0
        cmp.l   (%a3),%d0
        bne     1f
        moveq   #1,%d0
        rts
1:      moveq   #0,%d0
        rts

| The target D4-D7 (local top, left, bottom, right, longs) clipped by the
| pixel map's bounds, visRgn and clipRgn. Out: D0 -1 (N) when the blitter
| cannot do it (both regions complex, too many rectangles), 0 (Z) when
| nothing is drawn, 1 with D4-D7 the target in pixels and the clip list
| loaded.
clip:
clip_target:
        lea     6(%a3),%a0
        bsr     isect
        suba.l  %a1,%a1                 | the complex region, if any
        moveq   #0,%d2
        movea.l 24(%a2),%a0
        movea.l (%a0),%a0
        bsr     regbox
        movea.l 28(%a2),%a0
        movea.l (%a0),%a0
        bsr     regbox
        tst.w   %d2
        bne     8f
        cmp.l   %d4,%d6
        ble     7f
        cmp.l   %d5,%d7
        ble     7f
        move.w  6(%a3),%d0
        ext.l   %d0
        sub.l   %d0,%d4
        sub.l   %d0,%d6
        move.w  8(%a3),%d0
        ext.l   %d0
        sub.l   %d0,%d5
        sub.l   %d0,%d7
        move.l  %a1,%d0
        bne     1f
        clr.l   BLT_CLIPS(%a4)
        moveq   #1,%d0
        rts
1:      bsr     walk
        tst.l   %d0
        bmi     8f
        beq     7f
        move.l  %d0,BLT_CLIPS(%a4)
        moveq   #1,%d0
        rts
7:      moveq   #0,%d0
        rts
8:      moveq   #-1,%d0
        rts

| A0 = a region: its box clips the target; a complex one is kept in A1
| (D2 set when there are two).
regbox:
        cmpi.w  #10,(%a0)
        beq     2f
        move.l  %a1,%d0
        beq     1f
        moveq   #1,%d2
1:      movea.l %a0,%a1
2:      addq.l  #2,%a0
| D4-D7 clipped by the rectangle at A0 (words).
isect:
        move.w  (%a0)+,%d0
        ext.l   %d0
        cmp.l   %d0,%d4
        bge     1f
        move.l  %d0,%d4
1:      move.w  (%a0)+,%d0
        ext.l   %d0
        cmp.l   %d0,%d5
        bge     2f
        move.l  %d0,%d5
2:      move.w  (%a0)+,%d0
        ext.l   %d0
        cmp.l   %d0,%d6
        ble     3f
        move.l  %d0,%d6
3:      move.w  (%a0)+,%d0
        ext.l   %d0
        cmp.l   %d0,%d7
        ble     4f
        move.l  %d0,%d7
4:      rts

| The complex region A1 within the target D4-D7 (pixels) into the clip
| list: its scan lines' inversion points accumulated (each line's list
| toggles the points of the lines above it), every band between two lines
| a row of rectangles between successive pairs of points. Out: D0 the
| rectangles, -1 too many.
        .set    W_OY, -2
        .set    W_OX, -4
        .set    W_PREV, -6
        .set    W_COUNT, -8
        .set    W_T, -10
        .set    W_L, -12
        .set    W_B, -14
        .set    W_R, -16
        .set    MAXX, 64
        .set    W_CUR, -16-2*MAXX
        .set    W_NXT, -16-4*MAXX
walk:
        link    %a6,#-16-4*MAXX
        movem.l %d3-%d7/%a2-%a3,-(%sp)
        move.w  6(%a3),W_OY(%a6)        | bounds.top: local = pixel + bounds
        move.w  8(%a3),W_OX(%a6)
        move.w  %d4,%d0
        add.w   W_OY(%a6),%d0
        move.w  %d0,W_T(%a6)
        move.w  %d6,%d0
        add.w   W_OY(%a6),%d0
        move.w  %d0,W_B(%a6)
        move.w  %d5,%d0
        add.w   W_OX(%a6),%d0
        move.w  %d0,W_L(%a6)
        move.w  %d7,%d0
        add.w   W_OX(%a6),%d0
        move.w  %d0,W_R(%a6)
        clr.w   W_COUNT(%a6)
        lea     W_CUR(%a6),%a2
        lea     W_NXT(%a6),%a3
        moveq   #0,%d3                  | points on the current line
        lea     10(%a1),%a1
w_line:
        move.w  (%a1)+,%d1              | the next line's v
        cmpi.w  #0x7FFF,%d1
        beq     w_end
        tst.w   %d3
        beq     w_merge
        bsr     band
        bmi     w_over
w_merge:
        | The current points toggled by this line's into the other buffer.
        movea.l %a2,%a0
        move.w  %d3,%d2
        moveq   #0,%d5
m_next: move.w  (%a1),%d4
        cmpi.w  #0x7FFF,%d4
        beq     m_rest
        tst.w   %d2
        beq     m_line
        move.w  (%a0),%d6
        cmp.w   %d4,%d6
        blt     m_cur
        beq     m_both
m_line: cmpi.w  #MAXX,%d5
        bcc     w_over
        move.w  %d4,(%a3,%d5.w*2)
        addq.w  #1,%d5
        addq.l  #2,%a1
        bra     m_next
m_cur:  cmpi.w  #MAXX,%d5
        bcc     w_over
        move.w  %d6,(%a3,%d5.w*2)
        addq.w  #1,%d5
        addq.l  #2,%a0
        subq.w  #1,%d2
        bra     m_next
m_both: addq.l  #2,%a0
        subq.w  #1,%d2
        addq.l  #2,%a1
        bra     m_next
m_rest: addq.l  #2,%a1                  | past the line's $7FFF
1:      tst.w   %d2
        beq     2f
        cmpi.w  #MAXX,%d5
        bcc     w_over
        move.w  (%a0)+,(%a3,%d5.w*2)
        addq.w  #1,%d5
        subq.w  #1,%d2
        bra     1b
2:      exg     %a2,%a3
        move.w  %d5,%d3
        move.w  %d1,W_PREV(%a6)
        cmp.w   W_B(%a6),%d1
        blt     w_line
w_end:  moveq   #0,%d0
        move.w  W_COUNT(%a6),%d0
        bra     w_out
w_over: moveq   #-1,%d0
w_out:  movem.l (%sp)+,%d3-%d7/%a2-%a3
        unlk    %a6
        rts

| The band from W_PREV to D1 (local v) of the points at A2 (D3 of them)
| within the target, into the clip list. Out: D0 0, -1 (N) when full.
band:
        move.l  %d1,-(%sp)
        move.w  W_PREV(%a6),%d4
        cmp.w   W_T(%a6),%d4
        bge     1f
        move.w  W_T(%a6),%d4
1:      move.w  %d1,%d5
        cmp.w   W_B(%a6),%d5
        ble     2f
        move.w  W_B(%a6),%d5
2:      cmp.w   %d4,%d5
        ble     6f
        movea.l %a2,%a0
        move.w  %d3,%d2
        lsr.w   #1,%d2
        bra     5f
3:      move.w  (%a0)+,%d6
        move.w  (%a0)+,%d7
        cmp.w   W_L(%a6),%d6
        bge     4f
        move.w  W_L(%a6),%d6
4:      cmp.w   W_R(%a6),%d7
        ble     41f
        move.w  W_R(%a6),%d7
41:     cmp.w   %d6,%d7
        ble     5f
        moveq   #0,%d0
        move.w  W_COUNT(%a6),%d0
        cmpi.w  #MAX_CLIPS,%d0
        bcc     7f
        lsl.l   #3,%d0
        move.w  %d4,%d1
        sub.w   W_OY(%a6),%d1
        swap    %d1
        move.w  %d6,%d1
        sub.w   W_OX(%a6),%d1
        move.l  %d1,CLIP(%a4,%d0.l)
        move.w  %d5,%d1
        sub.w   W_OY(%a6),%d1
        swap    %d1
        move.w  %d7,%d1
        sub.w   W_OX(%a6),%d1
        move.l  %d1,CLIP+4(%a4,%d0.l)
        addq.w  #1,W_COUNT(%a6)
5:      dbra    %d2,3b
6:      move.l  (%sp)+,%d1
        moveq   #0,%d0
        rts
7:      move.l  (%sp)+,%d1
        moveq   #-1,%d0
        rts

| The blitter's destination: the pixel map A3 and the rectangle D4-D7
| (pixels), the pattern aligned to the port's local coordinates.
dst_regs:
        moveq   #0,%d0
        move.w  32(%a3),%d0
        move.l  %d0,BLT_DEPTH(%a4)
        move.l  (%a3),%d0
        sub.l   v_vram(%pc),%d0
        move.l  %d0,BLT_DST(%a4)
        moveq   #0,%d0
        move.w  4(%a3),%d0
        andi.w  #0x3FFF,%d0
        move.l  %d0,BLT_DST_STRIDE(%a4)
        move.l  %d5,BLT_DX(%a4)
        move.l  %d4,BLT_DY(%a4)
        move.l  %d7,%d0
        sub.l   %d5,%d0
        move.l  %d0,BLT_W(%a4)
        move.l  %d6,%d0
        sub.l   %d4,%d0
        move.l  %d0,BLT_H(%a4)
        move.w  8(%a3),%d0
        ext.l   %d0
        move.l  %d0,BLT_PATX(%a4)
        move.l  6(%a3),%d0
        swap    %d0
        ext.l   %d0
        move.l  %d0,BLT_PATY(%a4)
        rts

| Whether the rectangle D4-D7 (pixels) of the pixel map A3 is under
| MIN_BYTES: there setting up the blitter over NuBus, and hiding the cursor,
| costs about as much as QuickDraw's own drawing. Out: C set when it is.
small:
        move.l  %d7,%d0
        sub.l   %d5,%d0
        move.l  %d6,%d1
        sub.l   %d4,%d1
        mulu.l  %d1,%d0
        moveq   #0,%d1
        move.w  32(%a3),%d1
        mulu.l  %d1,%d0                 | bits
        cmpi.l  #MIN_BYTES*8,%d0
        rts

| The cursor hidden over D4-D7 (pixels: global coordinates on the only
| screen). Keeps D3-D7 and A2-A4.
shield:
        move.w  %d7,-(%sp)
        move.w  %d6,-(%sp)
        move.w  %d5,-(%sp)
        move.w  %d4,-(%sp)
        pea     (%sp)                   | shieldRect
        clr.l   -(%sp)                  | offsetPt (0, 0)
        .short  _ShieldCursor
        addq.l  #8,%sp
        rts

| Until the blitter is done.
wait:
1:      move.l  BLT_STATUS(%a4),%d0
        btst    #0,%d0
        bne     1b
        rts
        .balign 4
patch_end:

| The native patch's routine descriptor (its procedure set once prepared)
| and the fragment's container, copied together into the system heap.
        .balign 4
ppc_rd: .short  0xAAFE                  | _MixedModeMagic
        .byte   7, 0                    | version, flags
        .long   0
        .byte   0, 0                    | reserved, selectorInfo
        .short  0                       | routineCount - 1
        .long   0x3F1                   | procInfo
        .byte   0, 1                    | reserved, kPowerPCISA
        .short  4                       | kUseNativeISA
        .long   0                       | the procedure: install's vector
        .long   0, 0
ppc_pef:
        .incbin "../bin/livadixcard_accel_pef.bin"
ppc_end:
ppc_name:
        .byte   29
        .ascii  "Livadix Accelerated QuickDraw"
        .balign 2
