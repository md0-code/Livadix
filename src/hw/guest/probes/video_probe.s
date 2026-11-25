| Video probe (tests/verify.py, os-boot): the main screen's video driver
| asked for the Display Manager's Status calls (Designing Cards and Drivers
| for the Macintosh Family, 3rd ed., chapter 9, and Apple's video driver
| interface: csCode 10 GetCurMode, 12 GetConnection, 13 GetModeTiming, 16
| GetPreferredConfiguration). Position independent; run by mc_hw_trace
| --run-code, which prints the last 512 bytes (res):
|   +0 "VIDP" once done
|   +4 GetCurMode's result, +8 its csData (the display mode ID)
|   +12 GetConnection's result, +16 its VDDisplayConnectInfoRec (16 bytes:
|       csDisplayType word, csConnectTaggedType and csConnectTaggedData
|       bytes, csConnectFlags, csDisplayComponent, csConnectReserved)
|   +32 GetModeTiming's result for the current mode, +36 its VDTimingInfoRec
|       (20 bytes: csTimingMode, csTimingReserved, csTimingFormat,
|       csTimingData, csTimingFlags)
|   +56 GetModeTiming's result for display mode 127 (none)
|   +60 GetPreferredConfiguration's result, +64 its VDSwitchInfoRec
|       (csMode word, csData long, csPage word, csBaseAddr long)
|
| Inside Macintosh V, "Graphics Devices" (_GetMainDevice $AA2A, a GDevice's
| gdRefNum at 0); Inside Macintosh: Devices, chapter 1 (_Status $A005, $A205
| immediate; ioCRefNum at 24, csCode at 26, csParam at 28).
        .text
start:
        lea     res(%pc),%a4
        subq.l  #4,%sp
        .short  0xAA2A                  | _GetMainDevice
        movea.l (%sp)+,%a0
        movea.l (%a0),%a0
        move.w  (%a0),%d7               | gdRefNum
        moveq   #10,%d6                 | GetCurMode
        lea     rec(%pc),%a3
        bsr     status
        move.l  %d0,4(%a4)
        move.l  2(%a3),8(%a4)
        moveq   #12,%d6                 | GetConnection
        lea     16(%a4),%a3
        bsr     status
        move.l  %d0,12(%a4)
        moveq   #13,%d6                 | GetModeTiming, the current mode
        lea     36(%a4),%a3
        move.l  8(%a4),(%a3)
        bsr     status
        move.l  %d0,32(%a4)
        lea     rec(%pc),%a3            | and mode 127
        move.l  #127,(%a3)
        bsr     status
        move.l  %d0,56(%a4)
        moveq   #16,%d6                 | GetPreferredConfiguration
        lea     64(%a4),%a3
        bsr     status
        move.l  %d0,60(%a4)
        move.l  #0x56494450,(%a4)       | "VIDP"
done:   bra.s   done

| Status D6 to the driver D7 with csParam A3; D0 the result (long).
status:
        lea     pb(%pc),%a0
        moveq   #50 / 2 - 1,%d0
1:      clr.w   (%a0)+
        dbra    %d0,1b
        lea     pb(%pc),%a0
        move.w  %d7,24(%a0)             | ioCRefNum
        move.w  %d6,26(%a0)             | csCode
        move.l  %a3,28(%a0)             | csParam
        .short  0xA205                  | _Status, immediate
        ext.l   %d0
        rts

        .even
pb:     .space  50
rec:    .space  32
res:    .space  512
