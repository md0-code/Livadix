| AppleShare probe (tests/verify.py, check afp-share): at the Finder, with
| the volume "PC" of the server "Livadix" mounted through the Chooser (the
| gate's mouse clicks), the Macintosh finds the volume, lists its root,
| reads "hello.txt", writes "From Mac" (type TEXT, creator LVDX, both
| forks), then waits for "added.txt" to appear.
| Run by mc_hw_trace --live-code with interrupts enabled. It first moves
| itself into a block of the system heap (the Memory Manager hands out
| where mc_hw_trace put it while the AppleShare client loads its code)
| and says where at $FC, as mc_hw_trace asks; its results and its
| `bra.s *' are then the copy's. The long at
| offset 4 is unused, the long at 8 holds the ticks to wait for
| added.txt. The results are the last 512 bytes (res), "AFPP" in the first
| long once the program is done:
|   +8 0 once the volume is found (else _GetVolInfo's error), +12 its
|   vRefNum, +16 entries listed, +20 _Open hello.txt's, +24 _Read's,
|   +28 bytes read, +32 _Create's, +36 _SetFileInfo's, +40 the data
|   fork's _Write, +44 the resource fork's _Write, +48 _FlushVol's,
|   +52 the root's modification date before, +56 _GetFileInfo
|   added.txt's last result, +60 ticks waited for it, +64 the bytes read
|   (64), +128 the root's modification date after, +132 entries listed
|   after, +140 the data fork's _Read back,
|   +256 the names listed first (Pascal strings, up to 248 bytes).
|
| Sources: Inside Macintosh: Files (Apple, 1992), chapter 2: the calls are
| made asynchronously (bit 10 of the trap word; ioResult at 16 positive
| while one runs), _GetVolInfo by ioVolIndex (28), PBGetCatInfo (_HFSDispatch selector 9: ioFDirIndex at 28, ioFlAttrib
| at 30, ioDirID at 48, ioDrMdDat at 76), the file parameter block
| (ioFlFndrInfo at 32: fdType, fdCreator), _Create, _Open, _OpenRF,
| _Read, _Write, _Close, _GetFileInfo, _SetFileInfo, _FlushVol and their
| parameter blocks (ioNamePtr 18, ioVRefNum 22, ioRefNum 24, ioPermssn
| 27, ioBuffer 32, ioReqCount 36, ioActCount 40, ioPosMode 44).
        .text
start:
        bra.w   main
flags:  .long   0
waitfor:.long   3600
main:
        lea     start(%pc),%a3          | where mc_hw_trace put us
        move.l  #end-start,%d0
        .short  0xA51E                  | _NewPtrSys
        move.l  %a0,%d0
        beq.s   1f                      | no room: run in place
        move.l  %a0,%a1
        move.l  %a3,%a2
        move.w  #(end-start)/4-1,%d1
2:      move.l  (%a2)+,(%a1)+
        dbra    %d1,2b
        move.l  %a0,-(%sp)
        moveq   #0,%d0
        .short  0xA198                  | _HWPriv: flush the instruction cache
        move.l  (%sp)+,%a0
        move.l  %a0,0xFC                | the copy, for mc_hw_trace
        jmp     run-start(%a0)
1:
run:
        lea     res(%pc),%a5
| The volume "PC", mounted through the Chooser: _GetVolInfo by index.
        moveq   #1,%d6
1:      bsr     clear
        lea     name(%pc),%a1
        move.l  %a1,18(%a0)
        move.w  %d6,28(%a0)             | ioVolIndex
        .short  0xA407                  | async _GetVolInfo
        bsr     fmwait
        ext.l   %d0
        move.l  %d0,8(%a5)
        bne     done                    | no such volume (nsvErr)
        addq.w  #1,%d6
        lea     name(%pc),%a1
        cmp.b   #2,(%a1)+
        bne.s   1b
        cmp.b   #'P',(%a1)+
        bne.s   1b
        cmp.b   #'C',(%a1)
        bne.s   1b
        move.w  22(%a0),%d7             | its vRefNum
        move.w  %d7,12+2(%a5)
| The root's entries, their names kept.
        lea     256(%a5),%a4
        bsr     list
        move.l  %d6,16(%a5)
        bsr     rootdate
        move.l  %d0,52(%a5)
| hello.txt: open, read up to 64 bytes, close.
        bsr     clear
        lea     hello(%pc),%a1
        move.l  %a1,18(%a0)
        move.w  %d7,22(%a0)
        move.b  #1,27(%a0)              | read
        .short  0xA400                  | async _Open
        bsr     fmwait
        ext.l   %d0
        move.l  %d0,20(%a5)
        bne.s   2f
        move.w  24(%a0),%d5
        lea     64(%a5),%a1
        move.l  %a1,32(%a0)
        move.l  #64,36(%a0)
        clr.w   44(%a0)
        .short  0xA402                  | async _Read
        bsr     fmwait
        ext.l   %d0
        move.l  %d0,24(%a5)
        move.l  40(%a0),28(%a5)
        move.w  %d5,24(%a0)
        .short  0xA401                  | async _Close
        bsr     fmwait
2:
| From Mac: create, type and creator, the data fork, the resource fork.
        bsr     clear
        bsr     frommac
        .short  0xA408                  | async _Create
        bsr     fmwait
        ext.l   %d0
        move.l  %d0,32(%a5)
        bsr     clear
        bsr     frommac
        .short  0xA40C                  | async _GetFileInfo
        bsr     fmwait
        move.l  #0x54455854,32(%a0)     | 'TEXT'
        move.l  #0x4C564458,36(%a0)     | 'LVDX'
        bsr     frommac
        .short  0xA40D                  | async _SetFileInfo
        bsr     fmwait
        ext.l   %d0
        move.l  %d0,36(%a5)
        bsr     clear
        bsr     frommac
        move.b  #3,27(%a0)              | read and write
        .short  0xA400                  | async _Open
        bsr     fmwait
        tst.w   %d0
        bne.s   3f
        move.w  24(%a0),%d5
        lea     message(%pc),%a1
        move.l  %a1,32(%a0)
        move.l  #msgend-message,36(%a0)
        clr.w   44(%a0)
        .short  0xA403                  | async _Write
        bsr     fmwait
        ext.l   %d0
        move.l  %d0,40(%a5)
| ... and read back from the start
        lea     back(%pc),%a1
        move.l  %a1,32(%a0)
        move.l  #msgend-message,36(%a0)
        move.w  #1,44(%a0)              | fsFromStart
        clr.l   46(%a0)
        .short  0xA402                  | async _Read
        bsr     fmwait
        ext.l   %d0
        move.l  %d0,140(%a5)
        move.w  %d5,24(%a0)
        .short  0xA401                  | async _Close
        bsr     fmwait
3:      bsr     clear
        bsr     frommac
        move.b  #3,27(%a0)
        .short  0xA40A                  | async _OpenRF
        bsr     fmwait
        tst.w   %d0
        bne.s   4f
        move.w  24(%a0),%d5
| 600 bytes of resource fork: k * 7 for k = 0..599
        lea     rsrc(%pc),%a1
        moveq   #0,%d0
        move.w  #599,%d1
5:      move.b  %d0,(%a1)+
        addq.b  #7,%d0
        dbra    %d1,5b
        lea     rsrc(%pc),%a1
        move.l  %a1,32(%a0)
        move.l  #600,36(%a0)
        clr.w   44(%a0)
        .short  0xA403                  | async _Write
        bsr     fmwait
        ext.l   %d0
        move.l  %d0,44(%a5)
        move.w  %d5,24(%a0)
        .short  0xA401                  | async _Close
        bsr     fmwait
4:      bsr     clear
        move.w  %d7,22(%a0)
        .short  0xA413                  | async _FlushVol
        bsr     fmwait
        ext.l   %d0
        move.l  %d0,48(%a5)
| added.txt: asked once a second until it is there or the time is up.
        move.l  0x16A,%d4               | Ticks
        move.l  %d4,%d3
        add.l   waitfor(%pc),%d3
6:      bsr     clear
        lea     added(%pc),%a1
        move.l  %a1,18(%a0)
        move.w  %d7,22(%a0)
        .short  0xA40C                  | async _GetFileInfo
        bsr     fmwait
        ext.l   %d0
        move.l  %d0,56(%a5)
        beq.s   7f
        move.l  #60,%d1
        bsr     pause
        cmp.l   0x16A,%d3
        bhi.s   6b
7:      move.l  0x16A,%d0
        sub.l   %d4,%d0
        move.l  %d0,60(%a5)
        bsr     rootdate
        move.l  %d0,128(%a5)
        sub.l   %a4,%a4
        bsr     list
        move.l  %d6,132(%a5)
done:
        move.l  #0x41465050,(%a5)       | "AFPP"
stop:   bra.s   stop

| A0 = the cleared parameter block.
clear:
        lea     pb(%pc),%a0
        moveq   #31,%d0
1:      clr.l   (%a0)+
        dbra    %d0,1b
        lea     pb(%pc),%a0
        rts

| ioNamePtr = "From Mac", ioVRefNum = the volume.
frommac:
        lea     frommacname(%pc),%a1
        move.l  %a1,18(%a0)
        move.w  %d7,22(%a0)
        rts

| D1 ticks pass, giving the time to the rest of the system.
pause:
        move.l  0x16A,%d2
        add.l   %d1,%d2
1:      move.l  %d2,-(%sp)
        bsr     yield
        move.l  (%sp)+,%d2
        cmp.l   0x16A,%d2
        bhi.s   1b
        rts

| Waits for the asynchronous File Manager call of the block at A0 (the
| AppleShare client completes its work while the system runs); D0 = its
| result.
fmwait:
1:      move.w  16(%a0),%d0             | ioResult
        ble.s   2f
        move.l  %a0,-(%sp)
        bsr     yield
        move.l  (%sp)+,%a0
        bra.s   1b
2:      rts

| _SystemTask: the drivers' and desk accessories' periodic work.
yield:
        .short  0xA9B4                  | _SystemTask
        rts

| D6 = the root's entries (PBGetCatInfo by index); their names appended
| at A4 while A4 is not 0 and they fit.
list:
        moveq   #0,%d6
1:      bsr     clear
        lea     name(%pc),%a1
        move.l  %a1,18(%a0)
        move.w  %d7,22(%a0)
        move.w  %d6,28(%a0)
        addq.w  #1,28(%a0)              | ioFDirIndex
        move.l  #2,48(%a0)              | ioDirID: the root
        moveq   #9,%d0
        .short  0xA660                  | async _HFSDispatch: GetCatInfo
        bsr     fmwait
        tst.w   %d0
        bne.s   3f
        addq.l  #1,%d6
        move.l  %a4,%d0
        beq.s   1b
        lea     name(%pc),%a1
        moveq   #0,%d0
        move.b  (%a1),%d0
        lea     1(%a4,%d0.w),%a2
        lea     res+504(%pc),%a3
        cmp.l   %a3,%a2
        bhi.s   1b
2:      move.b  (%a1)+,(%a4)+
        dbra    %d0,2b
        bra.s   1b
3:      rts

| D0 = the root's modification date (PBGetCatInfo, ioFDirIndex -1).
rootdate:
        bsr     clear
        lea     name(%pc),%a1
        move.l  %a1,18(%a0)
        move.w  %d7,22(%a0)
        move.w  #-1,28(%a0)
        move.l  #2,48(%a0)
        moveq   #9,%d0
        .short  0xA660
        bsr     fmwait
        move.l  76(%a0),%d0             | ioDrMdDat
        rts

        .balign 4
hello:  .byte   9
        .ascii  "hello.txt"
added:  .byte   9
        .ascii  "added.txt"
frommacname:
        .byte   8
        .ascii  "From Mac"
        .balign 2
        .balign 2
message:.ascii  "written by the Macintosh\r"
msgend:
        .balign 4
back:   .space  32
name:   .space  64
event:  .space  16
pb:     .space  128
rsrc:   .space  600
        .balign 16
res:    .space  512
end:
