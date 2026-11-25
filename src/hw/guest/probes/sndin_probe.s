/* Livadix guest sound input check for the 68k Macintoshes. Inside
 * Macintosh: Sound, chapter 3, Sound Input Manager: SPBOpenDevice,
 * SPBGetDeviceInfo, SPBRecord and SPBCloseDevice on the default input
 * device in its own format. The results (a block starting 'SNDI'): +4 the
 * step reached, +8 SPBOpenDevice's result, +10 the sample size ('ssiz'),
 * +12 the channels ('chan'), +14 SPBRecord's result, +16 the sample rate
 * ('srat', Fixed), +20 the bytes recorded, +24 the SPB's error,
 * +26 SPBCloseDevice's result, +28/+29 the lowest and highest byte
 * recorded, +30 SPBGetIndexedDevice(1)'s result (the device then opened by
 * that name; the name's first 24 bytes at +32). */
        .text
        move.l  #0x2400,%d0
        .short  0xa71e                  /* _NewPtrSysClear */
        tst.l   %a0
        beq     done
        move.l  %a0,%a3
        lea     64(%a3),%a4             /* the SPB */
        move.l  #0x534e4449,(%a3)
        move.w  #1,4(%a3)
        clr.w   -(%sp)
        move.w  #1,-(%sp)
        pea     0x2200(%a3)             /* the name */
        pea     0x2300(%a3)             /* the icon handle, unused */
        move.l  #0x05140014,%d0         /* SPBGetIndexedDevice */
        .short  0xa800
        move.w  (%sp)+,30(%a3)
        lea     0x2200(%a3),%a0
        lea     32(%a3),%a1
        moveq   #5,%d0
4:      move.l  (%a0)+,(%a1)+
        dbra    %d0,4b
        clr.w   -(%sp)
        pea     0x2200(%a3)
        move.w  #1,-(%sp)               /* siWritePermission */
        move.l  %a4,-(%sp)
        move.l  #0x05180014,%d0         /* SPBOpenDevice */
        .short  0xa800
        move.w  (%sp)+,8(%a3)
        bne     done
        move.w  #2,4(%a3)
        move.l  #0x7373697a,%d1         /* 'ssiz' */
        lea     10(%a3),%a1
        bsr     get_info
        move.l  #0x6368616e,%d1         /* 'chan' */
        lea     12(%a3),%a1
        bsr     get_info
        move.l  #0x73726174,%d1         /* 'srat' */
        lea     16(%a3),%a1
        bsr     get_info
        move.w  #3,4(%a3)
        move.l  #8192,4(%a4)            /* count */
        clr.l   8(%a4)                  /* milliseconds */
        move.l  #8192,12(%a4)           /* bufferLength */
        lea     256(%a3),%a0
        move.l  %a0,16(%a4)             /* bufferPtr */
        clr.l   20(%a4)
        clr.l   24(%a4)
        clr.w   -(%sp)
        move.l  %a4,-(%sp)
        clr.w   -(%sp)                  /* synchronous */
        move.l  #0x03200014,%d0         /* SPBRecord */
        .short  0xa800
        move.w  (%sp)+,14(%a3)
        move.l  4(%a4),20(%a3)
        move.w  32(%a4),24(%a3)
        lea     256(%a3),%a0
        move.b  (%a0),%d0
        move.b  %d0,%d1
        move.w  #8191,%d2
1:      move.b  (%a0)+,%d3
        cmp.b   %d0,%d3
        bcc.s   2f
        move.b  %d3,%d0
2:      cmp.b   %d1,%d3
        bls.s   3f
        move.b  %d3,%d1
3:      dbra    %d2,1b
        move.b  %d0,28(%a3)
        move.b  %d1,29(%a3)
        move.w  #4,4(%a3)
        clr.w   -(%sp)
        move.l  (%a4),-(%sp)
        move.l  #0x021c0014,%d0         /* SPBCloseDevice */
        .short  0xa800
        move.w  (%sp)+,26(%a3)
        move.w  #5,4(%a3)
done:   .short  0xa9f4                  /* _ExitToShell */
/* SPBGetDeviceInfo(refNum, selector D1, infoPtr A1) */
get_info:
        clr.w   -(%sp)
        move.l  (%a4),-(%sp)
        move.l  %d1,-(%sp)
        move.l  %a1,-(%sp)
        move.l  #0x06380014,%d0
        .short  0xa800
        move.w  (%sp)+,%d0
        rts
empty:  .word   0
