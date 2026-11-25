| Acceleration probe (tests/verify.py, check accel-equal): at the Finder,
| the Macintosh draws one fixed scene with Color QuickDraw into a colour
| port of its own over the whole screen, covering what the Livadix Display
| Card's accelerated QuickDraw takes (src/hw/guest/declrom/livadixcard_accel.s)
| and what it leaves to QuickDraw: rectangles painted, filled, erased and
| inverted with the five standard patterns and others, in colours, at odd
| coordinates and partly off the screen; a moved origin (pattern
| alignment); a clip rectangle; an oval clip region and an oval visRgn (the
| region walk); CopyBits of the screen to itself in every direction,
| overlapping or not, partly onto off the screen; and pen modes, a hidden
| pen and colourized copies it leaves to QuickDraw. Run as mc_paced --draw
| runs draw_probe.s (the long at offset 4 the times the scene is drawn); the
| screen it leaves is compared between video.acceleration on and off. The
| results are the last 512 bytes (res): +0 "DRAW" once done, +4 the ticks
| the scenes took, +20 the port's rectangle, +28 the screen's pixel size.
|
| The cursor is hidden first (_HideCursor $A852, Inside Macintosh I,
| "QuickDraw"), so the screen compared is the drawing alone.
|
| Inside Macintosh I, "QuickDraw" (the QDGlobals before thePort: white -8,
| black -16, gray -24, ltGray -32, dkGray -40; _GetPort $A874, _SetPort
| $A873, _ClosePort $A87D, _SetOrigin $A878, _SetClip $A879, _ClipRect
| $A87B, _BackPat $A87C, _HidePen $A896, _ShowPen $A897, _PenMode $A89C,
| _PenPat $A89D, _PenNormal $A89E, _PaintRect $A8A2, _EraseRect $A8A3,
| _InvertRect $A8A4, _FillRect $A8A5, _FrameOval $A8B7, _NewRgn $A8D8,
| _DisposeRgn $A8D9, _OpenRgn $A8DA, _CloseRgn $A8DB, _CopyRgn $A8DC,
| _RectRgn $A8DF, _CopyBits $A8EC; pen modes patOr 9, patXor 10), Inside
| Macintosh V, "Color QuickDraw" (_OpenCPort $AA00, _RGBForeColor $AA14,
| _RGBBackColor $AA15; a CGrafPort's visRgn at 24), low memory Ticks ($16A).
        .text
start:
        bra.w   main
count:  .long   1                       | scenes, replaced by --live-arg
main:
        .short  0xA852                  | _HideCursor: the screen without it
        lea     res(%pc),%a4
        pea     oldport(%pc)
        .short  0xA874                  | _GetPort
        pea     port(%pc)
        .short  0xAA00                  | _OpenCPort
        lea     port+16(%pc),%a0
        move.l  (%a0),20(%a4)
        move.l  4(%a0),24(%a4)
        movea.l port+2(%pc),%a0
        movea.l (%a0),%a0
        move.w  32(%a0),30(%a4)
        move.l  0x16A.w,%d0
        move.l  %d0,-(%sp)
        move.l  count(%pc),%d0
        move.l  %d0,-(%sp)
1:      bsr     scene
        subq.l  #1,(%sp)
        bne.s   1b
        addq.l  #4,%sp
        move.l  0x16A.w,%d0
        sub.l   (%sp)+,%d0
        move.l  %d0,4(%a4)
        pea     port(%pc)
        .short  0xA87D                  | _ClosePort
        move.l  oldport(%pc),-(%sp)
        .short  0xA873                  | _SetPort
        move.l  #0x44524157,(%a4)       | "DRAW"
done:   bra.s   done

| A0 = the pattern D0 (mod 6): white, black, gray, ltGray, dkGray, stripes.
getpat:
        andi.l  #0xFFFF,%d0
        divu.w  #6,%d0
        swap    %d0
        cmpi.w  #5,%d0
        beq     1f
        addq.w  #1,%d0
        lsl.w   #3,%d0
        neg.w   %d0
        ext.l   %d0
        movea.l (%a5),%a0
        adda.l  %d0,%a0
        rts
1:      lea     stripes(%pc),%a0
        rts

| tmp = the rectangle at A3 moved by D0 down, D1 right.
offs:
        lea     tmp(%pc),%a1
        move.w  (%a3),(%a1)
        add.w   %d0,(%a1)
        move.w  2(%a3),2(%a1)
        add.w   %d1,2(%a1)
        move.w  4(%a3),4(%a1)
        add.w   %d0,4(%a1)
        move.w  6(%a3),6(%a1)
        add.w   %d1,6(%a1)
        rts

| Black on white again.
colours_off:
        pea     rgb_black(%pc)
        .short  0xAA14                  | _RGBForeColor
        pea     rgb_white(%pc)
        .short  0xAA15                  | _RGBBackColor
        rts

| The rectangles of the table, each painted, filled, filled in colour,
| inverted and erased.
rects:
        lea     table(%pc),%a3
        moveq   #0,%d6
1:      cmpi.w  #0x7FFF,(%a3)
        beq     9f
        move.l  %d6,%d0
        bsr     getpat
        move.l  %a0,-(%sp)
        .short  0xA89D                  | _PenPat
        move.l  %a3,-(%sp)
        .short  0xA8A2                  | _PaintRect
        moveq   #5,%d0
        moveq   #9,%d1
        bsr     offs
        pea     tmp(%pc)
        move.l  %d6,%d0
        addq.l  #1,%d0
        bsr     getpat
        move.l  %a0,-(%sp)
        .short  0xA8A5                  | _FillRect
        lea     colours(%pc),%a0
        move.l  %d6,%d0
        andi.l  #3,%d0
        mulu.w  #6,%d0
        pea     (%a0,%d0.l)
        .short  0xAA14                  | _RGBForeColor
        lea     colours+6(%pc),%a0
        move.l  %d6,%d0
        andi.l  #3,%d0
        mulu.w  #6,%d0
        pea     (%a0,%d0.l)
        .short  0xAA15                  | _RGBBackColor
        moveq   #-7,%d0
        moveq   #13,%d1
        bsr     offs
        pea     tmp(%pc)
        move.l  %d6,%d0
        addq.l  #2,%d0
        bsr     getpat
        move.l  %a0,-(%sp)
        .short  0xA8A5                  | _FillRect, colourized
        moveq   #3,%d0
        moveq   #-2,%d1
        bsr     offs
        move.l  %d6,%d0
        addq.l  #3,%d0
        bsr     getpat
        move.l  %a0,-(%sp)
        .short  0xA89D                  | _PenPat
        pea     tmp(%pc)
        .short  0xA8A2                  | _PaintRect, colourized
        bsr     colours_off
        moveq   #11,%d0
        moveq   #-4,%d1
        bsr     offs
        pea     tmp(%pc)
        .short  0xA8A4                  | _InvertRect
        movea.l (%a5),%a0
        pea     -32(%a0)
        .short  0xA87C                  | _BackPat ltGray
        moveq   #17,%d0
        moveq   #21,%d1
        bsr     offs
        pea     tmp(%pc)
        .short  0xA8A3                  | _EraseRect
        movea.l (%a5),%a0
        pea     -8(%a0)
        .short  0xA87C                  | _BackPat white
        addq.l  #8,%a3
        addq.l  #1,%d6
        bra     1b
9:      .short  0xA89E                  | _PenNormal
        rts

| CopyBits of the screen to itself for each pair of the copy table.
copies:
        lea     ctable(%pc),%a3
1:      cmpi.w  #0x7FFF,(%a3)
        beq     9f
        pea     port+2(%pc)
        pea     port+2(%pc)
        pea     (%a3)
        pea     8(%a3)
        clr.w   -(%sp)                  | srcCopy
        clr.l   -(%sp)
        .short  0xA8EC                  | _CopyBits
        lea     16(%a3),%a3
        bra     1b
9:      rts

scene:
        movea.l (%a5),%a0
        pea     port+16(%pc)
        pea     -8(%a0)
        .short  0xA8A5                  | _FillRect the screen white
        bsr     rects
        bsr     copies
        | A moved origin: pattern alignment.
        move.w  #-3,-(%sp)
        move.w  #5,-(%sp)
        .short  0xA878                  | _SetOrigin
        bsr     rects
        bsr     copies
        move.w  #13,-(%sp)
        move.w  #-6,-(%sp)
        .short  0xA878
        bsr     rects
        clr.l   -(%sp)
        .short  0xA878                  | _SetOrigin(0, 0)
        | A clip rectangle.
        pea     cliprect(%pc)
        .short  0xA87B                  | _ClipRect
        bsr     rects
        bsr     copies
        pea     wide(%pc)
        .short  0xA87B
        | An oval clip region.
        clr.l   -(%sp)
        .short  0xA8D8                  | _NewRgn
        lea     rgn(%pc),%a0
        move.l  (%sp)+,(%a0)
        .short  0xA8DA                  | _OpenRgn
        pea     oval(%pc)
        .short  0xA8B7                  | _FrameOval
        move.l  rgn(%pc),-(%sp)
        .short  0xA8DB                  | _CloseRgn
        move.l  rgn(%pc),-(%sp)
        .short  0xA879                  | _SetClip
        movea.l (%a5),%a0
        pea     port+16(%pc)
        pea     -40(%a0)
        .short  0xA8A5                  | _FillRect dkGray
        bsr     rects
        bsr     copies
        pea     wide(%pc)
        .short  0xA87B
        | An oval visRgn, as a partly covered window has.
        move.l  rgn(%pc),-(%sp)
        move.l  port+24(%pc),-(%sp)
        .short  0xA8DC                  | _CopyRgn
        bsr     rects
        bsr     copies
        | Both regions complex: QuickDraw's.
        clr.l   -(%sp)
        .short  0xA8D8                  | _NewRgn
        lea     rgn2(%pc),%a0
        move.l  (%sp)+,(%a0)
        .short  0xA8DA                  | _OpenRgn
        pea     oval2(%pc)
        .short  0xA8B7                  | _FrameOval
        move.l  rgn2(%pc),-(%sp)
        .short  0xA8DB                  | _CloseRgn
        move.l  rgn2(%pc),-(%sp)
        .short  0xA879                  | _SetClip
        bsr     rects
        pea     wide(%pc)
        .short  0xA87B
        move.l  port+24(%pc),-(%sp)
        pea     port+16(%pc)
        .short  0xA8DF                  | _RectRgn visRgn = portRect
        move.l  rgn(%pc),-(%sp)
        .short  0xA8D9                  | _DisposeRgn
        move.l  rgn2(%pc),-(%sp)
        .short  0xA8D9
        | Left to QuickDraw: pen modes, a hidden pen, colourized copies.
        move.w  #9,-(%sp)
        .short  0xA89C                  | _PenMode patOr
        bsr     rects
        move.w  #10,-(%sp)
        .short  0xA89C                  | _PenMode patXor
        bsr     rects
        .short  0xA89E                  | _PenNormal
        .short  0xA896                  | _HidePen
        bsr     rects
        .short  0xA897                  | _ShowPen
        pea     colours(%pc)
        .short  0xAA14
        bsr     copies
        bsr     colours_off
        rts

oldport: .long  0
rgn:    .long   0
rgn2:   .long   0
tmp:    .short  0, 0, 0, 0
wide:   .short  -32767, -32767, 32767, 32767
cliprect: .short 37, 29, 251, 333
oval:   .short  -20, 40, 300, 470
oval2:  .short  60, -30, 380, 260
rgb_black: .short 0, 0, 0
rgb_white: .short 0xFFFF, 0xFFFF, 0xFFFF
colours: .short 0xFFFF, 0, 0
        .short  0, 0x8000, 0xFFFF
        .short  0x3333, 0xCCCC, 0x1111
        .short  0xFFFF, 0xFFFF, 0
        .short  0x4444, 0, 0x8888
stripes: .byte  0x81, 0x42, 0x24, 0x18, 0x18, 0x24, 0x42, 0x81
| Rectangles (top, left, bottom, right).
table:  .short  3, 5, 41, 97
        .short  17, 101, 63, 230
        .short  -9, 200, 30, 333
        .short  50, -6, 140, 61
        .short  77, 77, 78, 300
        .short  90, 123, 211, 124
        .short  101, 151, 187, 259
        .short  133, 33, 260, 170
        .short  160, 290, 333, 515
        .short  211, 7, 222, 501
        .short  240, 250, 251, 251
        .short  255, 61, 330, 205
        .short  300, 400, 360, 530
        .short  5, 470, 120, 513
        .short  0x7FFF
| Copies: source rectangle, destination rectangle.
ctable: .short  10, 10, 110, 150,   13, 15, 113, 155
        .short  60, 60, 160, 200,   55, 57, 155, 197
        .short  20, 300, 90, 420,   20, 292, 90, 412
        .short  20, 300, 90, 420,   20, 308, 90, 428
        .short  150, 20, 250, 140,  158, 20, 258, 140
        .short  150, 20, 250, 140,  142, 20, 242, 140
        .short  5, 5, 45, 85,       200, 300, 240, 380
        .short  250, 30, 300, 100,  -15, 100, 35, 170
        .short  280, 450, 330, 530, 100, 200, 150, 280
        .short  30, 3, 300, 7,      30, 4, 300, 8
        .short  0x7FFF
port:   .space  128
        .balign 1024
        .space  512
res:    .space  512
