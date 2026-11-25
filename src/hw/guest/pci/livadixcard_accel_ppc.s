# Livadix — the Livadix Display Card's accelerated QuickDraw for the PowerPC
# (the card's registers in
# src/hw/livadix_card.h): the native counterpart of the 68k patch
# (src/hw/guest/declrom/livadixcard_accel.s), with the same conditions, the
# same operations on the same blitter and the same fall-through.
#
# A code fragment of its own: tools/pcirom.py packs it into a PEF container
# (Mac OS Runtime Architectures, chapter 8; livadixcard_accel_pef.yaml), code
# from code_base to code_end as section 0, data from data_base to data_end
# as section 1, its main symbol the transition vector of `install`. Both
# cards prepare it with the Code Fragment Manager's GetMemFragment (Inside
# Macintosh: PowerPC System Software, chapter 3) from a copy in the system
# heap and call install(vram, registers) through the main symbol: the NuBus
# card's SecondaryInit (livadixcard_accel.s) when Gestalt 'sysa' is
# gestaltPowerPC, the PCI card's native driver (livadixvideo_pci.s) at
# Initialize.
#
# install keeps the routines in front of two of QuickDraw's bottlenecks
# (Inside Macintosh: Imaging With QuickDraw, "Customizing QuickDraw's
# Low-Level Routines"): StdRect ($A8A0) and StdBits ($A8EB), with
# GetToolTrapAddress and SetToolTrapAddress (Inside Macintosh: Operating
# System Utilities, "Trap Manager"), the trap table entries being routine
# descriptors for the native routines (Inside Macintosh: PowerPC System
# Software, chapter 2, "Mixed Mode Manager": RoutineDescriptor, its
# version 7, one RoutineRecord of kPowerPCISA with kUseNativeISA, the
# procedure a transition vector; ProcInfo uppQDRectProcInfo $340,
# uppQDBitsProcInfo $EFC0, kPascalStackBased). A 68k caller reaches them
# through the Mixed Mode Manager, a native one directly; what they leave
# goes to the routine that was there before through CallUniversalProc.
# Patches the System and the extensions put in front of them later stay in
# front: some take calls of their own away from the screen (Mac OS 8.1's,
# while the Finder drags an icon, a CopyBits whose port is the screen's but
# whose pixels go elsewhere: drawn on the screen instead, it left a copy of
# the icon in its top left corner), and only they know which. Mac OS 9.0.4's
# StdRect patch, which draws most ports itself, then leaves the blitter few
# calls.
#
# The calls taken, as the 68k patch's (its header gives the sources and
# the reasons): thePort a colour port drawing into the card's screen, the
# only screen, recording nothing, patStretch clear; StdRect's paint
# (patCopy), erase and fill with an old-style pattern in fgColor/bkColor
# (at 1 and 2 bits not when two distinct RGB colours share an index),
# invert outside the hilite mode, with the pen shown; StdBits' srcCopy of
# the card's screen to itself, no mask region, no stretching, the same
# depth and colour table, black on white, the source inside its bounds;
# none under 1 KB of pixels, and no fill or copy over 8 KB (`small`);
# clipped by the pixel map's bounds, visRgn and
# clipRgn, at most one of the two regions complex (its scan lines walked
# into the clip list, at most 128 rectangles); the cursor shielded
# (ShieldCursor, ShowCursor). thePort is the QuickDraw globals' first
# field: *(*CurrentA5) (low memory $904; Inside Macintosh I, "QuickDraw"),
# HiliteMode $938 and DeviceList $8A8 as the 68k patch reads them.
#
# Calling conventions (Mac OS Runtime Architectures, chapter 4): r1 the
# stack, its linkage area with the LR at 8 and the TOC at 20, the frame
# allocated with stwu; r2 the TOC, the data section (every data reference
# r2-relative); arguments in r3-r10, the result in r3; r13-r31 kept by every
# callee (saved with stmw at the top of the frame); CR fields 2-4 kept
# (unused here); a call into another fragment through its transition vector
# with the TOC saved at 20(r1) and reloaded after (CALL). The internal
# routines below are leaves on fixed registers:
#   r31 thePort, r30 its pixel map, r29 the card's registers, r28 its VRAM,
#   r24-r27 the target (top, left, bottom, right), r23 the operation,
#   r22 the complex region, r21 set when there are two, r20 clip's LR,
#   r14-r18 the call's arguments, r16-r19 the pattern and colours (StdRect)
#   or r19 the source pixel map (StdBits).
#
# Assembler: GNU as for PowerPC (binutils), -mbig -mppc32 -mregnames; built
# by tools/guest_build.py into src/hw/guest/bin/livadixcard_accel_ppc.bin,
# whose first 16 bytes give the code size, the data size, the offset of the
# main symbol's transition vector in the data and 0.

        .set    CurrentA5, 0x0904
        .set    DeviceList, 0x08A8
        .set    HiliteMode, 0x0938
        .set    R_DEPTH, 0x028
        .set    R_STRIDE, 0x02C
        .set    R_BASE, 0x030
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
        .set    MAX_BYTES, 8192         # fill and copy: not above
        .set    OP_FILL, 1
        .set    OP_INVERT, 2
        .set    OP_COPY, 3
        .set    TRAP_STDRECT, 0xA8A0
        .set    TRAP_STDBITS, 0xA8EB
        .set    uppQDRectProcInfo, 0x340
        .set    uppQDBitsProcInfo, 0xEFC0

# The import table, in livadixcard_accel_pef.yaml's order (InterfaceLib).
        .set    IMP_CallUniversalProc, 0
        .set    IMP_GetToolTrapAddress, 4
        .set    IMP_SetToolTrapAddress, 8
        .set    IMP_ShieldCursor, 12
        .set    IMP_ShowCursor, 16
        .set    IMPORTS, 5

# The patches' frame: the linkage area and eight words of parameters (0-55),
# the locals, r13-r31 at the top.
        .set    MAXX, 64                # inversion points on a scan line
        .set    L_SHIELD, 56            # 8: the rectangle shielded
        .set    L_DV, 64                # StdBits: the source's offset, v
        .set    L_DH, 68                #   and h
        .set    L_LR, 72                # go's return address
        .set    W_SAVE, 76              # 32: walk's r13-r19 and r23
        .set    W_CUR, 108              # 2 * MAXX: one line's points
        .set    W_NXT, 108 + 2 * MAXX   # and the next
        .set    SAVE, 108 + 4 * MAXX    # r13-r31
        .set    FRAME, (SAVE + 76 + 15) & ~15

# --- macros -------------------------------------------------------------------

        .macro  LA rd, sym
        addi    \rd, r2, \sym - data_base
        .endm

        .macro  CALL imp
        lwz     r12, \imp(r2)
        stw     r2, 20(r1)
        lwz     r0, 0(r12)
        lwz     r2, 4(r12)
        mtctr   r0
        bctrl
        lwz     r2, 20(r1)
        .endm

        .macro  PROLOGUE
        mflr    r0
        stw     r0, 8(r1)
        stwu    r1, -FRAME(r1)
        stmw    r13, SAVE(r1)
        .endm

        .macro  EPILOGUE
        lmw     r13, SAVE(r1)
        addi    r1, r1, FRAME
        lwz     r0, 8(r1)
        mtlr    r0
        blr
        .endm

        .text

        .long   code_end - code_base
        .long   data_end - data_base
        .long   install_tvec - data_base
        .long   0

code_base:

# --- install(vram, registers): the main symbol -----------------------------------

# r3 = the card's VRAM, r4 = its registers (logical addresses). r3 = 0.
install:
        mflr    r0
        stw     r0, 8(r1)
        stwu    r1, -64(r1)
        stw     r3, v_vram - data_base(r2)
        stw     r4, v_regs - data_base(r2)
        lwz     r0, v_rect - data_base(r2)
        cmpwi   r0, 0
        bne     1f                      # installed already
        li      r3, 0
        ori     r3, r3, TRAP_STDRECT
        CALL    IMP_GetToolTrapAddress
        stw     r3, v_rect - data_base(r2)
        li      r3, 0
        ori     r3, r3, TRAP_STDBITS
        CALL    IMP_GetToolTrapAddress
        stw     r3, v_bits - data_base(r2)
        LA      r3, rd_rect
        li      r4, 0
        ori     r4, r4, TRAP_STDRECT
        CALL    IMP_SetToolTrapAddress
        LA      r3, rd_bits
        li      r4, 0
        ori     r4, r4, TRAP_STDBITS
        CALL    IMP_SetToolTrapAddress
1:      li      r3, 0
        addi    r1, r1, 64
        lwz     r0, 8(r1)
        mtlr    r0
        blr

# --- StdRect(verb: GrafVerb; r: Rect) ----------------------------------------------

# r3 = the verb, r4 = the rectangle. r13 the routine a call goes on to.
rect_patch:
        PROLOGUE
        lwz     r13, v_rect - data_base(r2)
        mr      r14, r3
        mr      r15, r4
        bl      check
        cmpwi   r3, 0
        beq     r_fall
        lha     r0, 66(r31)             # pnVis: a hidden pen draws nothing
        cmpwi   r0, 0
        blt     r_fall
        clrlwi  r3, r14, 24
        cmpwi   r3, 3
        beq     r_invert
        cmpwi   r3, 1
        beq     r_paint
        cmpwi   r3, 2
        beq     r_erase
        cmpwi   r3, 4
        bne     r_fall
        lwz     r11, 62(r31)            # fill: fillPixPat
        b       r_pat
r_paint:
        lha     r0, 56(r31)             # pnMode patCopy
        cmpwi   r0, 8
        bne     r_fall
        lwz     r11, 58(r31)            # pnPixPat
        b       r_pat
r_erase:
        lwz     r11, 32(r31)            # bkPixPat
r_pat:
        cmpwi   r11, 0
        beq     r_fall
        lwz     r11, 0(r11)
        lha     r0, 0(r11)              # patType 0: an old-style pattern
        cmpwi   r0, 0
        bne     r_fall
        lwz     r11, 6(r11)             # patData: its eight bytes
        cmpwi   r11, 0
        beq     r_fall
        lwz     r11, 0(r11)
        cmpwi   r11, 0
        beq     r_fall
        lwz     r16, 0(r11)
        lwz     r17, 4(r11)
        lwz     r18, 80(r31)            # fgColor
        lwz     r19, 84(r31)            # bkColor
        lhz     r0, 32(r30)
        cmplwi  r0, 2                   # at 1 and 2 bits, two distinct RGB
        bgt     r_fill                  # colours that map to one index are
        cmpw    r18, r19                # separated by QuickDraw (patExpand)
        bne     r_fill
        lwz     r3, 36(r31)
        lwz     r4, 42(r31)
        cmpw    r3, r4
        bne     r_fall
        lhz     r3, 40(r31)
        lhz     r4, 46(r31)
        cmpw    r3, r4
        bne     r_fall
r_fill:
        li      r23, OP_FILL
        b       r_draw
r_invert:
        lbz     r0, HiliteMode(0)
        andi.   r0, r0, 0x80
        beq     r_fall                  # the hilite mode: QuickDraw's
        li      r16, -1
        li      r17, -1
        # Every bit of an index; a direct pixel's colour bits only.
        lhz     r0, 32(r30)
        li      r18, -1
        cmpwi   r0, 16
        bne     1f
        li      r18, 0x7FFF
1:      cmpwi   r0, 32
        bne     2f
        lis     r18, 0x00FF
        ori     r18, r18, 0xFFFF
2:      li      r19, 0
        li      r23, OP_INVERT
r_draw:
        lha     r24, 0(r15)
        lha     r25, 2(r15)
        lha     r26, 4(r15)
        lha     r27, 6(r15)
        bl      clip
        cmpwi   r3, 0
        blt     r_fall
        beq     r_done
        bl      small
        cmpwi   r3, 0
        bne     r_fall
        bl      dst_regs
        stw     r16, BLT_PAT0(r29)
        stw     r17, BLT_PAT1(r29)
        stw     r18, BLT_FG(r29)
        stw     r19, BLT_BG(r29)
        stw     r23, BLT_OP(r29)
        sth     r24, L_SHIELD(r1)
        sth     r25, L_SHIELD+2(r1)
        sth     r26, L_SHIELD+4(r1)
        sth     r27, L_SHIELD+6(r1)
        bl      go
r_done:
        EPILOGUE
r_fall:
        mr      r3, r13
        li      r4, uppQDRectProcInfo
        mr      r5, r14
        mr      r6, r15
        CALL    IMP_CallUniversalProc
        b       r_done

# --- StdBits(srcBits, srcRect, dstRect, mode, maskRgn) -----------------------------

bits_patch:
        PROLOGUE
        lwz     r13, v_bits - data_base(r2)
        mr      r14, r3                 # srcBits
        mr      r15, r4                 # srcRect
        mr      r16, r5                 # dstRect
        mr      r17, r6                 # mode
        mr      r18, r7                 # maskRgn
        bl      check
        cmpwi   r3, 0
        beq     b_fall
        cmpwi   r18, 0                  # no mask region
        bne     b_fall
        clrlwi. r0, r17, 16             # srcCopy
        bne     b_fall
        lwz     r0, 36(r31)             # rgbFgColor black
        cmpwi   r0, 0
        bne     b_fall
        lhz     r0, 40(r31)
        cmpwi   r0, 0
        bne     b_fall
        lwz     r0, 42(r31)             # rgbBkColor white
        cmpwi   r0, -1
        bne     b_fall
        lhz     r0, 46(r31)
        cmplwi  r0, 0xFFFF
        bne     b_fall
        mr      r19, r14
        lha     r0, 4(r19)
        cmpwi   r0, 0
        bge     b_fall                  # a BitMap
        andi.   r0, r0, 0xC000
        cmplwi  r0, 0xC000
        bne     1f
        lwz     r19, 0(r19)             # a colour port's portPixMap
        lwz     r19, 0(r19)
1:      lwz     r0, 0(r19)              # the same screen
        lwz     r3, 0(r30)
        cmpw    r0, r3
        bne     b_fall
        lhz     r0, 4(r19)
        clrlwi  r0, r0, 18
        lhz     r3, 4(r30)
        clrlwi  r3, r3, 18
        cmpw    r0, r3
        bne     b_fall
        lhz     r0, 32(r19)
        lhz     r3, 32(r30)
        cmpw    r0, r3
        bne     b_fall
        cmplwi  r0, 8
        bgt     2f
        lwz     r3, 42(r19)             # indexed: the same colour table
        cmpwi   r3, 0
        beq     b_fall
        lwz     r3, 0(r3)
        cmpwi   r3, 0
        beq     b_fall
        lwz     r3, 0(r3)               # its ctSeed
        lwz     r4, 42(r30)
        cmpwi   r4, 0
        beq     b_fall
        lwz     r4, 0(r4)
        cmpwi   r4, 0
        beq     b_fall
        lwz     r4, 0(r4)
        cmpw    r3, r4
        bne     b_fall
2:      lha     r3, 4(r16)              # no stretching
        lha     r0, 0(r16)
        subf    r3, r0, r3
        lha     r4, 4(r15)
        lha     r0, 0(r15)
        subf    r4, r0, r4
        cmpw    r3, r4
        bne     b_fall
        lha     r3, 6(r16)
        lha     r0, 2(r16)
        subf    r3, r0, r3
        lha     r4, 6(r15)
        lha     r0, 2(r15)
        subf    r4, r0, r4
        cmpw    r3, r4
        bne     b_fall
        # QuickDraw reads a source outside its pixel map's bounds as the
        # memory there: only a source inside them is the blitter's.
        lha     r0, 0(r15)
        lha     r3, 6(r19)
        cmpw    r0, r3
        blt     b_fall
        lha     r0, 2(r15)
        lha     r3, 8(r19)
        cmpw    r0, r3
        blt     b_fall
        lha     r0, 4(r15)
        lha     r3, 10(r19)
        cmpw    r0, r3
        bgt     b_fall
        lha     r0, 6(r15)
        lha     r3, 12(r19)
        cmpw    r0, r3
        bgt     b_fall
        # The source pixel of a destination pixel: (srcRect.topLeft -
        # srcBounds.topLeft) - (dstRect.topLeft - dstBounds.topLeft) on.
        lha     r3, 0(r15)
        lha     r0, 6(r19)
        subf    r3, r0, r3
        lha     r0, 0(r16)
        subf    r3, r0, r3
        lha     r0, 6(r30)
        add     r3, r3, r0
        stw     r3, L_DV(r1)
        lha     r3, 2(r15)
        lha     r0, 8(r19)
        subf    r3, r0, r3
        lha     r0, 2(r16)
        subf    r3, r0, r3
        lha     r0, 8(r30)
        add     r3, r3, r0
        stw     r3, L_DH(r1)
        li      r23, OP_COPY
        lha     r24, 0(r16)             # the target: dstRect
        lha     r25, 2(r16)
        lha     r26, 4(r16)
        lha     r27, 6(r16)
        bl      clip
        cmpwi   r3, 0
        blt     b_fall
        beq     b_done
        bl      small
        cmpwi   r3, 0
        bne     b_fall
        bl      dst_regs
        stw     r23, BLT_OP(r29)
        lwz     r0, 0(r19)
        subf    r0, r28, r0
        stw     r0, BLT_SRC(r29)
        lhz     r0, 4(r19)
        clrlwi  r0, r0, 18
        stw     r0, BLT_SRC_STRIDE(r29)
        lwz     r4, L_DV(r1)
        lwz     r5, L_DH(r1)
        add     r0, r25, r5
        stw     r0, BLT_SX(r29)
        add     r0, r24, r4
        stw     r0, BLT_SY(r29)
        # The cursor off both rectangles: their union.
        mr      r6, r24
        mr      r7, r25
        mr      r8, r26
        mr      r9, r27
        cmpwi   r4, 0
        bge     1f
        add     r6, r6, r4
        b       2f
1:      add     r8, r8, r4
2:      cmpwi   r5, 0
        bge     3f
        add     r7, r7, r5
        b       4f
3:      add     r9, r9, r5
4:      sth     r6, L_SHIELD(r1)
        sth     r7, L_SHIELD+2(r1)
        sth     r8, L_SHIELD+4(r1)
        sth     r9, L_SHIELD+6(r1)
        bl      go
b_done:
        EPILOGUE
b_fall:
        mr      r3, r13
        lis     r4, uppQDBitsProcInfo >> 16
        ori     r4, r4, uppQDBitsProcInfo & 0xFFFF
        mr      r5, r14
        mr      r6, r15
        mr      r7, r16
        mr      r8, r17
        mr      r9, r18
        CALL    IMP_CallUniversalProc
        b       b_done

# --- the internal routines ----------------------------------------------------------

# The blitter started with the cursor hidden over L_SHIELD (global
# coordinates on the only screen), then the cursor shown again. Called from
# a patch's frame; keeps r13-r31.
go:
        mflr    r0
        stw     r0, L_LR(r1)
        addi    r3, r1, L_SHIELD
        li      r4, 0                   # offsetPt (0, 0)
        CALL    IMP_ShieldCursor
        eieio
        li      r0, 1
        stw     r0, BLT_GO(r29)
        eieio
1:      lwz     r0, BLT_STATUS(r29)     # until the blitter is done
        andi.   r0, r0, 1
        bne     1b
        CALL    IMP_ShowCursor
        lwz     r0, L_LR(r1)
        mtlr    r0
        blr

# Whether thePort draws into the card's screen, the only screen, as a colour
# port recording nothing. Out: r3 0 when not; else 1 with r31 the port, r30
# its pixel map, r29 the card's registers, r28 its VRAM.
check:
        lwz     r11, CurrentA5(0)
        cmpwi   r11, 0
        beq     9f
        lwz     r11, 0(r11)
        cmpwi   r11, 0
        beq     9f
        lwz     r31, 0(r11)             # thePort
        cmpwi   r31, 0
        beq     9f
        lhz     r0, 6(r31)              # portVersion: a CGrafPort
        andi.   r0, r0, 0xC000
        cmplwi  r0, 0xC000
        bne     9f
        lwz     r0, 92(r31)             # picSave
        cmpwi   r0, 0
        bne     9f
        lwz     r0, 96(r31)             # rgnSave
        cmpwi   r0, 0
        bne     9f
        lwz     r0, 100(r31)            # polySave
        cmpwi   r0, 0
        bne     9f
        lhz     r0, 90(r31)             # patStretch
        cmpwi   r0, 0
        bne     9f
        lwz     r30, 2(r31)
        lwz     r30, 0(r30)             # portPixMap
        lwz     r29, v_regs - data_base(r2)
        lwz     r28, v_vram - data_base(r2)
        lwz     r3, 0(r30)
        subf    r3, r28, r3
        lwz     r0, R_BASE(r29)
        cmpw    r3, r0
        bne     9f
        lha     r0, 4(r30)
        cmpwi   r0, 0
        bge     9f
        clrlwi  r3, r0, 18
        lwz     r0, R_STRIDE(r29)
        cmpw    r3, r0
        bne     9f
        lhz     r3, 32(r30)
        lwz     r0, R_DEPTH(r29)
        cmpw    r3, r0
        bne     9f
        lwz     r11, DeviceList(0)
        lwz     r11, 0(r11)
        lwz     r0, 30(r11)             # gdNextGD: the only screen
        cmpwi   r0, 0
        bne     9f
        lwz     r11, 22(r11)
        lwz     r11, 0(r11)             # gdPMap
        lwz     r3, 0(r11)
        lwz     r0, 0(r30)
        cmpw    r3, r0
        bne     9f
        li      r3, 1
        blr
9:      li      r3, 0
        blr

# The target r24-r27 (local) clipped by the pixel map's bounds, visRgn and
# clipRgn. Out: r3 -1 when the blitter cannot do it (both regions complex,
# too many rectangles), 0 when nothing is drawn, 1 with r24-r27 the target
# in pixels and the clip list loaded.
clip:
        mflr    r20
        addi    r3, r30, 6
        bl      isect
        li      r22, 0                  # the complex region, if any
        li      r21, 0
        lwz     r3, 24(r31)
        lwz     r3, 0(r3)
        bl      regbox
        lwz     r3, 28(r31)
        lwz     r3, 0(r3)
        bl      regbox
        mtlr    r20
        cmpwi   r21, 0
        bne     8f
        cmpw    r24, r26
        bge     7f
        cmpw    r25, r27
        bge     7f
        lha     r0, 6(r30)
        subf    r24, r0, r24
        subf    r26, r0, r26
        lha     r0, 8(r30)
        subf    r25, r0, r25
        subf    r27, r0, r27
        cmpwi   r22, 0
        bne     1f
        li      r0, 0
        stw     r0, BLT_CLIPS(r29)
        li      r3, 1
        blr
1:      bl      walk
        mtlr    r20
        cmpwi   r3, 0
        blt     8f
        beq     7f
        stw     r3, BLT_CLIPS(r29)
        li      r3, 1
        blr
7:      li      r3, 0
        blr
8:      li      r3, -1
        blr

# r3 = a region: its box clips the target; a complex one is kept in r22
# (r21 set when there are two).
regbox:
        lhz     r0, 0(r3)
        cmpwi   r0, 10
        beq     2f
        cmpwi   r22, 0
        beq     1f
        li      r21, 1
1:      mr      r22, r3
2:      addi    r3, r3, 2
# r24-r27 clipped by the rectangle at r3 (halfwords).
isect:
        lha     r0, 0(r3)
        cmpw    r24, r0
        bge     1f
        mr      r24, r0
1:      lha     r0, 2(r3)
        cmpw    r25, r0
        bge     2f
        mr      r25, r0
2:      lha     r0, 4(r3)
        cmpw    r26, r0
        ble     3f
        mr      r26, r0
3:      lha     r0, 6(r3)
        cmpw    r27, r0
        ble     4f
        mr      r27, r0
4:      blr

# The complex region r22 within the target r24-r27 (pixels) into the clip
# list: its scan lines' inversion points accumulated (each line's list
# toggles the points of the lines above it), every band between two lines a
# row of rectangles between successive pairs of points. Out: r3 the
# rectangles, -1 too many. Keeps r13-r31 but r21 and r22.
#   r4, r5 bounds.top and .left (local = pixel + bounds); r6-r9 the target
#   in local coordinates (top, left, bottom, right); r10, r11 the current
#   and the next line's points; r12 their number on the current line; r14
#   the rectangles; r15 the line's v; r17 the previous line's v; merging,
#   r16 the points written, r18 and r19 the current points left; r0, r3,
#   r13, r21 and r23 scratch.
walk:
        stw     r13, W_SAVE(r1)
        stw     r14, W_SAVE+4(r1)
        stw     r15, W_SAVE+8(r1)
        stw     r16, W_SAVE+12(r1)
        stw     r17, W_SAVE+16(r1)
        stw     r18, W_SAVE+20(r1)
        stw     r19, W_SAVE+24(r1)
        stw     r23, W_SAVE+28(r1)
        lha     r4, 6(r30)
        lha     r5, 8(r30)
        add     r6, r24, r4
        add     r7, r25, r5
        add     r8, r26, r4
        add     r9, r27, r5
        li      r14, 0
        addi    r10, r1, W_CUR
        addi    r11, r1, W_NXT
        li      r12, 0
        li      r17, 0
        addi    r22, r22, 10
w_line:
        lha     r15, 0(r22)             # the next line's v
        addi    r22, r22, 2
        cmpwi   r15, 0x7FFF
        beq     w_end
        cmpwi   r12, 0
        beq     w_merge
        # The band from r17 to r15 of the current points within the target.
        mr      r16, r17
        cmpw    r16, r6
        bge     1f
        mr      r16, r6
1:      mr      r21, r15
        cmpw    r21, r8
        ble     2f
        mr      r21, r8
2:      cmpw    r21, r16
        ble     w_merge
        mr      r18, r10
        srwi    r19, r12, 1
3:      cmpwi   r19, 0
        beq     w_merge
        subi    r19, r19, 1
        lha     r0, 0(r18)
        lha     r3, 2(r18)
        addi    r18, r18, 4
        cmpw    r0, r7
        bge     4f
        mr      r0, r7
4:      cmpw    r3, r9
        ble     5f
        mr      r3, r9
5:      cmpw    r3, r0
        ble     3b
        cmpwi   r14, MAX_CLIPS
        bge     w_over
        slwi    r23, r14, 3
        add     r23, r23, r29
        subf    r13, r5, r0             # (top, left) in pixels
        subf    r0, r4, r16
        rlwimi  r13, r0, 16, 0, 15
        stw     r13, CLIP(r23)
        subf    r13, r5, r3             # (bottom, right)
        subf    r0, r4, r21
        rlwimi  r13, r0, 16, 0, 15
        stw     r13, CLIP+4(r23)
        addi    r14, r14, 1
        b       3b
w_merge:
        # The current points toggled by this line's into the other buffer.
        mr      r18, r10
        mr      r19, r12
        li      r16, 0
m_next: lha     r0, 0(r22)
        cmpwi   r0, 0x7FFF
        beq     m_rest
        cmpwi   r19, 0
        beq     m_line
        lha     r3, 0(r18)
        cmpw    r3, r0
        blt     m_cur
        beq     m_both
m_line: cmpwi   r16, MAXX
        bge     w_over
        slwi    r21, r16, 1
        sthx    r0, r11, r21
        addi    r16, r16, 1
        addi    r22, r22, 2
        b       m_next
m_cur:  cmpwi   r16, MAXX
        bge     w_over
        slwi    r21, r16, 1
        sthx    r3, r11, r21
        addi    r16, r16, 1
        addi    r18, r18, 2
        subi    r19, r19, 1
        b       m_next
m_both: addi    r18, r18, 2
        subi    r19, r19, 1
        addi    r22, r22, 2
        b       m_next
m_rest: addi    r22, r22, 2             # past the line's $7FFF
1:      cmpwi   r19, 0
        beq     2f
        cmpwi   r16, MAXX
        bge     w_over
        lha     r3, 0(r18)
        addi    r18, r18, 2
        slwi    r21, r16, 1
        sthx    r3, r11, r21
        addi    r16, r16, 1
        subi    r19, r19, 1
        b       1b
2:      mr      r21, r10
        mr      r10, r11
        mr      r11, r21
        mr      r12, r16
        mr      r17, r15
        cmpw    r15, r8
        blt     w_line
w_end:  mr      r3, r14
        b       w_out
w_over: li      r3, -1
w_out:  lwz     r13, W_SAVE(r1)
        lwz     r14, W_SAVE+4(r1)
        lwz     r15, W_SAVE+8(r1)
        lwz     r16, W_SAVE+12(r1)
        lwz     r17, W_SAVE+16(r1)
        lwz     r18, W_SAVE+20(r1)
        lwz     r19, W_SAVE+24(r1)
        lwz     r23, W_SAVE+28(r1)
        blr

# Whether the rectangle r24-r27 (pixels) of the pixel map r30 is left to
# QuickDraw: under MIN_BYTES, where setting up the blitter, and hiding the
# cursor, costs about as much as QuickDraw's own drawing (as in the 68k
# patch); and, for a fill or a copy (r23), over MAX_BYTES. The emulated
# Power Macintoshes charge the processor no bus time for its accesses to the
# card, so their native QuickDraw writes the screen faster than the
# blitter's modelled 266 MB/s fill and 133 MB/s copy and loses only a fixed
# cost per call, which the blitter beats for a small rectangle; an inversion, QuickDraw reading and writing each pixel,
# stays faster on the blitter at every size. Out: r3 1 when it is.
small:
        subf    r3, r25, r27
        subf    r0, r24, r26
        mullw   r3, r3, r0
        lhz     r0, 32(r30)
        mullw   r3, r3, r0              # bits
        cmpwi   r3, MIN_BYTES * 8
        blt     1f
        cmpwi   r23, OP_INVERT
        beq     2f
        lis     r0, (MAX_BYTES * 8) >> 16
        ori     r0, r0, (MAX_BYTES * 8) & 0xFFFF
        cmpw    r3, r0
        bgt     1f
2:      li      r3, 0
        blr
1:      li      r3, 1
        blr

# The blitter's destination: the pixel map r30 and the rectangle r24-r27
# (pixels), the pattern aligned to the port's local coordinates.
dst_regs:
        lhz     r0, 32(r30)
        stw     r0, BLT_DEPTH(r29)
        lwz     r0, 0(r30)
        subf    r0, r28, r0
        stw     r0, BLT_DST(r29)
        lhz     r0, 4(r30)
        clrlwi  r0, r0, 18
        stw     r0, BLT_DST_STRIDE(r29)
        stw     r25, BLT_DX(r29)
        stw     r24, BLT_DY(r29)
        subf    r0, r25, r27
        stw     r0, BLT_W(r29)
        subf    r0, r24, r26
        stw     r0, BLT_H(r29)
        lha     r0, 8(r30)
        stw     r0, BLT_PATX(r29)
        lha     r0, 6(r30)
        stw     r0, BLT_PATY(r29)
        blr

        .balign 16
code_end:

# --- data -----------------------------------------------------------------------

data_base:
import_table:
        .space  4 * IMPORTS
# Transition vectors [code, TOC], relocated by the sections' addresses.
install_tvec:
        .long   install - code_base, 0
rect_tvec:
        .long   rect_patch - code_base, 0
bits_tvec:
        .long   bits_patch - code_base, 0
# Routine descriptors (MixedMode.h): _MixedModeMagic $AAFE, version 7,
# flags 0, reserved, selectorInfo 0, routineCount 0; the routine record:
# procInfo, reserved, ISA kPowerPCISA (1), routineFlags kUseNativeISA (4),
# the procedure (its transition vector, relocated), reserved, selector 0.
        .macro  RD procinfo, tvec
        .short  0xAAFE
        .byte   7, 0
        .long   0
        .byte   0, 0
        .short  0
        .long   \procinfo
        .byte   0, 1
        .short  4
        .long   \tvec - data_base
        .long   0, 0
        .endm
rd_rect:   RD      uppQDRectProcInfo, rect_tvec     # the trap table's, from install
rd_bits:   RD      uppQDBitsProcInfo, bits_tvec
v_rect:   .long   0                     # StdRect at install
v_bits:   .long   0                     # StdBits at install
v_vram:   .long   0                     # the card's VRAM
v_regs:   .long   0                     # its registers
        .balign 16
data_end:
