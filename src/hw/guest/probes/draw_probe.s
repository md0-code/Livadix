| Drawing probe (tests/verify.py, check draw-speed): at the Finder, the
| Macintosh draws with Color QuickDraw into a colour port of its own over
| the whole screen and counts the ticks each kind of drawing takes. Run by
| mc_hw_trace --live-code with interrupts enabled. The long at offset 4 is
| the repeat count N (--live-arg). The results are the last 512 bytes (res):
|   +0 "DRAW" once done, +4 the ticks of N full-screen FillRects (black and
|   white patterns in turn), +8 of N full-screen CopyBits from the screen to
|   itself 8 pixels lower (srcCopy, as when a window moves or a list
|   scrolls), +12 of N full-screen InvertRects, +16 of 20 x N DrawStrings of
|   a 32-character string in the system font, +20 the port's rectangle
|   (top, left, bottom, right), +28 the screen's pixel size.
|
| Inside Macintosh I, "QuickDraw" (the QDGlobals before thePort: white at
| -8, black at -16; the GrafPort's portRect at 16, portBits at 2; _GetPort
| $A874, _SetPort $A873, _ClosePort $A87D, _FillRect $A8A5, _InvertRect
| $A8A4, _CopyBits $A8EC, _MoveTo $A893, _DrawString $A884), Inside
| Macintosh V, "Color QuickDraw" (_OpenCPort $AA00; a CGrafPort's
| portPixMap handle at 2 and portVersion at 6, its pixMap's pixelSize at
| 32; CopyBits takes &port->portBits of a CGrafPort), low memory Ticks
| ($16A).
        .text
start:
        bra.w   main
count:  .long   20                      | N, replaced by --live-arg
main:
        lea     res(%pc),%a4
        pea     oldport(%pc)
        .short  0xA874                  | _GetPort
        pea     port(%pc)
        .short  0xAA00                  | _OpenCPort: the whole screen, current
        lea     port+16(%pc),%a3        | portRect
        move.l  (%a3),20(%a4)
        move.l  4(%a3),24(%a4)
        movea.l port+2(%pc),%a0         | portPixMap
        movea.l (%a0),%a0
        move.w  32(%a0),30(%a4)         | pixelSize

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

        | CopyBits, the screen 8 pixels lower
        lea     r1(%pc),%a2
        move.l  (%a3),(%a2)
        move.l  4(%a3),4(%a2)
        subq.w  #8,4(%a2)               | src: top .. bottom - 8
        lea     r2(%pc),%a1
        move.l  (%a3),(%a1)
        move.l  4(%a3),4(%a1)
        addq.w  #8,(%a1)                | dst: top + 8 .. bottom
        move.l  0x16A.w,%d6
        move.l  count(%pc),%d7
3:      pea     port+2(%pc)
        pea     port+2(%pc)
        pea     r1(%pc)
        pea     r2(%pc)
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

        | DrawString, 20 x N lines down the screen
        move.l  0x16A.w,%d6
        move.l  count(%pc),%d7
        mulu.w  #20,%d7
5:      move.w  #8,-(%sp)               | h
        move.l  %d7,%d0
        andi.w  #31,%d0
        mulu.w  #12,%d0
        addi.w  #16,%d0
        move.w  %d0,-(%sp)              | v
        .short  0xA893                  | _MoveTo
        pea     text(%pc)
        .short  0xA884                  | _DrawString
        subq.l  #1,%d7
        bne.s   5b
        move.l  0x16A.w,%d0
        sub.l   %d6,%d0
        move.l  %d0,16(%a4)

        pea     port(%pc)
        .short  0xA87D                  | _ClosePort
        move.l  oldport(%pc),-(%sp)
        .short  0xA873                  | _SetPort
        move.l  #0x44524157,(%a4)       | "DRAW"
done:   bra.s   done

text:   .byte   32
        .ascii  "The quick brown fox jumps over t"
        .even
oldport: .long  0
r1:     .long   0, 0
r2:     .long   0, 0
port:   .space  128
        .balign 1024
        .space  512
res:    .space  512
