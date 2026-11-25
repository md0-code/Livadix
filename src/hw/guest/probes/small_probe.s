| Small-rectangle drawing probe (tests/verify.py, check draw-speed): at the
| Finder, the Macintosh draws with Color QuickDraw into a colour port of its
| own over the whole screen and counts the ticks each kind of drawing takes
| on a rectangle of 2 KB of pixels (32 lines of 512 / pixel size pixels, at
| (100, 100)), the size where the Livadix Display Card's native accelerated
| QuickDraw on a Power Macintosh goes to the blitter for every operation.
| Run as a startup application. The long at offset 4 is the repeat count N.
| The results are the last 512 bytes (res):
|   +0 "DRAW" once done, +4 the ticks of N FillRects (black and white
|   patterns in turn), +8 of N CopyBits from the screen to itself 8 lines
|   lower (srcCopy), +12 of N InvertRects, +16 0, +20 the rectangle (top,
|   left, bottom, right), +28 the screen's pixel size.
|
| The calls are draw_probe.s's: Inside Macintosh I, "QuickDraw" (the
| QDGlobals before thePort: white at -8, black at -16; _GetPort $A874,
| _SetPort $A873, _ClosePort $A87D, _FillRect $A8A5, _InvertRect $A8A4,
| _CopyBits $A8EC), Inside Macintosh V, "Color QuickDraw" (_OpenCPort $AA00;
| a CGrafPort's portPixMap handle at 2, its pixMap's pixelSize at 32), low
| memory Ticks ($16A).
        .text
start:
        bra.w   main
count:  .long   20                      | N
main:
        lea     res(%pc),%a4
        pea     oldport(%pc)
        .short  0xA874                  | _GetPort
        pea     port(%pc)
        .short  0xAA00                  | _OpenCPort: the whole screen, current
        movea.l port+2(%pc),%a0         | portPixMap
        movea.l (%a0),%a0
        moveq   #0,%d1
        move.w  32(%a0),%d1             | pixelSize
        move.w  %d1,30(%a4)
        lea     r1(%pc),%a3             | the rectangle: 32 lines, 2 KB
        move.w  #100,(%a3)
        move.w  #100,2(%a3)
        move.w  #132,4(%a3)
        move.l  #512,%d0
        divu.w  %d1,%d0
        addi.w  #100,%d0
        move.w  %d0,6(%a3)
        move.l  (%a3),20(%a4)
        move.l  4(%a3),24(%a4)
        lea     r2(%pc),%a2             | the copy's target: 8 lines lower
        move.l  (%a3),(%a2)
        move.l  4(%a3),4(%a2)
        addq.w  #8,(%a2)
        addq.w  #8,4(%a2)

        | FillRect, black and white in turn
        move.l  0x16A.w,%d6
        move.l  count(%pc),%d7
1:      move.l  %a3,-(%sp)
        movea.l (%a5),%a0
        lea     -16(%a0),%a1
        btst    #0,%d7
        beq.s   2f
        lea     -8(%a0),%a1
2:      move.l  %a1,-(%sp)
        .short  0xA8A5                  | _FillRect
        subq.l  #1,%d7
        bne.s   1b
        move.l  0x16A.w,%d0
        sub.l   %d6,%d0
        move.l  %d0,4(%a4)

        | CopyBits, the rectangle 8 lines lower
        move.l  0x16A.w,%d6
        move.l  count(%pc),%d7
3:      pea     port+2(%pc)
        pea     port+2(%pc)
        move.l  %a3,-(%sp)
        move.l  %a2,-(%sp)
        clr.w   -(%sp)                  | srcCopy
        clr.l   -(%sp)                  | no mask region
        .short  0xA8EC                  | _CopyBits
        subq.l  #1,%d7
        bne.s   3b
        move.l  0x16A.w,%d0
        sub.l   %d6,%d0
        move.l  %d0,8(%a4)

        | InvertRect
        move.l  0x16A.w,%d6
        move.l  count(%pc),%d7
4:      move.l  %a3,-(%sp)
        .short  0xA8A4                  | _InvertRect
        subq.l  #1,%d7
        bne.s   4b
        move.l  0x16A.w,%d0
        sub.l   %d6,%d0
        move.l  %d0,12(%a4)

        pea     port(%pc)
        .short  0xA87D                  | _ClosePort
        move.l  oldport(%pc),-(%sp)
        .short  0xA873                  | _SetPort
        move.l  #0x44524157,(%a4)       | "DRAW"
done:   bra.s   done

oldport: .long  0
r1:     .long   0, 0
r2:     .long   0, 0
port:   .space  128
        .balign 1024
        .space  512
res:    .space  512
