| Network probe (tests/verify.py, check net-boot): at the Finder, the
| Macintosh opens a TCP connection through its TCP/IP stack's MacTCP
| driver interface (.IPP: MacTCP itself, or Open Transport's MacTCP
| compatibility), sends a line, reads the answer and closes. Run by
| mc_hw_trace --live-code with interrupts enabled; the longs at offsets 4
| and 8 are the server's address and port (--live-arg); the results are the
| last 512 bytes (res), "NETP" in the first long once the program is done:
|   +4 _Open's result, +10 the driver's refNum, +12 ipctlGetAddr's result,
|   +16 our address, +20 our mask, +24 TCPCreate's result, +28 the stream,
|   +32 TCPActiveOpen's result, +36 the remote host, +40 TCPSend's, +44
|   TCPRcv's result, +48 the bytes received, +52 TCPClose's, +56
|   TCPRelease's, +60 the .ENET open's (flags), +64 the bytes received
|   (64), +128 the ipctlGetAddr attempts.
|
| The calls and their parameter blocks: MacTCP Programmer's Guide (Apple,
| 1989): ipctlGetAddr (csCode 15: ourAddress at 28, ourNetMask at 32);
| TCPCreate (30: rcvBuff 32, rcvBuffLen 36, notifyProc 40, userDataPtr 44),
| TCPActiveOpen (32: ulpTimeoutValue 32, ulpTimeoutAction 33,
| validityFlags 34, commandTimeoutValue 35, remoteHost 36, remotePort 40,
| localHost 42, localPort 46), TCPSend (34: pushFlag 35, wdsPtr 38),
| TCPRcv (37: commandTimeoutValue 32, rcvBuff 36, rcvBuffLen 40), TCPClose
| (38), TCPRelease (42: rcvBuff 32, rcvBuffLen 36); the stream pointer at
| 28 of a TCP block. Inside Macintosh II, "The Device Manager": _Open
| ($A000) and _Control ($A004), ioNamePtr at 18, ioResult at 16, ioRefNum
| and ioCRefNum at 24, csCode at 26.
        .text
start:
        bra.w   main
server: .long   0x0A000202              | 10.0.2.2, replaced by --live-arg
port:   .long   7                       | replaced by --live-arg
flags:  .long   0                       | bit 0: open .ENET first; bit 1: with OpenSlot, slot in bits 8-15, sResource in 16-23 (--live-arg)
main:
        lea     res(%pc),%a5
        move.l  flags(%pc),%d0
        btst    #0,%d0
        beq.s   1f
        lea     pb(%pc),%a0
        bsr     clear
        lea     enet(%pc),%a1
        move.l  %a1,18(%a0)
        move.l  flags(%pc),%d0
        btst    #1,%d0
        bne.s   2f
        .short  0xA000                  | _Open .ENET
        bra.s   3f
2:      lsr.l   #8,%d0
        move.b  %d0,34(%a0)             | ioSlot
        lsr.l   #8,%d0
        move.b  %d0,35(%a0)             | ioId
        .short  0xA200                  | _OpenSlot .ENET
3:      ext.l   %d0
        move.l  %d0,60(%a5)
1:
        lea     pb(%pc),%a0
        bsr     clear
        lea     ipp(%pc),%a1
        move.l  %a1,18(%a0)             | ioNamePtr
        .short  0xA000                  | _Open
        ext.l   %d0
        move.l  %d0,4(%a5)
        move.w  24(%a0),%d7             | the driver's reference number
        move.w  %d7,8+2(%a5)
        tst.l   %d0
        bne     done
| Our address: asked again every second, for up to a minute, while the
| stack loads and configures itself (DHCP included).
        move.l  0x16A,%d5               | Ticks
        add.l   #3600,%d5
4:      bsr     clear
        move.w  #15,%d1                 | ipctlGetAddr
        bsr     control
        move.l  %d0,12(%a5)
        move.l  28(%a0),16(%a5)
        move.l  32(%a0),20(%a5)
        addq.l  #1,128(%a5)             | attempts
        tst.l   %d0
        bne.s   5f
        tst.l   16(%a5)
        bne.s   6f
5:      move.l  0x16A,%d4
        add.l   #60,%d4
7:      .short  0xA9B4                  | _SystemTask
        cmp.l   0x16A,%d4
        bhi.s   7b
        cmp.l   0x16A,%d5
        bhi.s   4b
6:
| TCPCreate with an 8 KiB receive buffer.
        bsr     clear
        lea     rcvbuf(%pc),%a1
        move.l  %a1,32(%a0)
        move.l  #8192,36(%a0)
        move.w  #30,%d1
        bsr     control
        move.l  %d0,24(%a5)
        move.l  28(%a0),%d6             | the stream
        move.l  %d6,28(%a5)
        tst.l   %d0
        bne     done
| TCPActiveOpen to the server.
        bsr     clear
        move.l  %d6,28(%a0)
        move.b  #60,35(%a0)             | commandTimeoutValue, s
        move.l  server(%pc),36(%a0)
        move.l  port(%pc),%d0
        move.w  %d0,40(%a0)
        move.w  #32,%d1
        bsr     control
        move.l  %d0,32(%a5)
        move.l  36(%a0),36(%a5)         | remoteHost as opened
        tst.l   %d0
        bne     release
| TCPSend: one line.
        bsr     clear
        move.l  %d6,28(%a0)
        move.b  #1,35(%a0)              | pushFlag
        lea     wds(%pc),%a1
        lea     msg(%pc),%a2
        move.l  %a2,2(%a1)              | the buffer's address (position independent)
        move.l  %a1,38(%a0)
        move.w  #34,%d1
        bsr     control
        move.l  %d0,40(%a5)
| TCPRcv: the answer, up to 64 bytes.
        bsr     clear
        move.l  %d6,28(%a0)
        move.b  #30,32(%a0)             | commandTimeoutValue, s
        lea     64(%a5),%a1
        move.l  %a1,36(%a0)
        move.w  #64,40(%a0)
        move.w  #37,%d1
        bsr     control
        move.l  %d0,44(%a5)
        moveq   #0,%d0
        move.w  40(%a0),%d0             | bytes received
        move.l  %d0,48(%a5)
| TCPClose.
        bsr     clear
        move.l  %d6,28(%a0)
        move.w  #38,%d1
        bsr     control
        move.l  %d0,52(%a5)
release:
        bsr     clear
        move.l  %d6,28(%a0)
        lea     rcvbuf(%pc),%a1
        move.l  %a1,32(%a0)
        move.l  #8192,36(%a0)
        move.w  #42,%d1
        bsr     control
        move.l  %d0,56(%a5)
done:
        move.l  #0x4E455450,(%a5)       | "NETP"
stop:   bra.s   stop

| A0 = the cleared parameter block.
clear:
        lea     pb(%pc),%a0
        moveq   #31,%d0
1:      clr.l   (%a0)+
        dbra    %d0,1b
        lea     pb(%pc),%a0
        rts

| _Control of the driver: D1 = csCode; D0 = ioResult.
control:
        move.w  %d7,24(%a0)
        move.w  %d1,26(%a0)
        .short  0xA004                  | _Control
        ext.l   %d0
        rts

        .balign 4
ipp:    .byte   4
        .ascii  ".IPP"
enet:   .byte   5
        .ascii  ".ENET"
        .balign 4
pb:     .space  128
wds:    .short  msgend-msg              | one buffer, then the end
        .long   msg
        .short  0
        .long   0
msg:    .ascii  "hello from the Macintosh\r\n"
msgend:
        .balign 4
rcvbuf: .space  8192
        .balign 16
res:    .space  512
