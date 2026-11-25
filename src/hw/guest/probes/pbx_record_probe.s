/* Livadix guest recording check. Inside Macintosh: Sound, chapter 3,
 * Sound Input Manager: SPBOpenDevice, SPBSetDeviceInfo, SPBRecord and
 * SPBCloseDevice. The original input driver chooses gain and channel mix.
 * The result contains API status, byte count and signed sample range. */
        .text
        move.l  #0x2400,%d0
        .short  0xa71e
        tst.l   %a0
        beq     done
        move.l  %a0,%a3
        lea     64(%a3),%a4
        move.l  #0x50425852,(%a3)
        move.w  #1,4(%a3)
        clr.w   -(%sp)
        pea     empty(%pc)
        move.w  #1,-(%sp)
        move.l  %a4,-(%sp)
        move.l  #0x05180014,%d0
        .short  0xa800
        move.w  (%sp)+,8(%a3)
        bne     done
        move.w  #2,4(%a3)
        move.w  #16,16(%a3)
        move.l  #0x7373697a,%d1
        bsr     set_info
        move.w  %d0,10(%a3)
        move.w  #2,16(%a3)
        move.l  #0x6368616e,%d1
        bsr     set_info
        move.w  %d0,12(%a3)
        move.w  #3,4(%a3)
        move.l  #8192,4(%a4)
        move.l  #8192,12(%a4)
        lea     256(%a3),%a0
        move.l  %a0,16(%a4)
        move.l  #0x7bad5eed,(%a0)
        clr.w   -(%sp)
        move.l  %a4,-(%sp)
        clr.w   -(%sp)
        move.l  #0x03200014,%d0
        .short  0xa800
        move.w  (%sp)+,14(%a3)
        move.w  32(%a4),18(%a3)
        move.l  4(%a4),20(%a3)
        move.l  256(%a3),24(%a3)
        cmp.l   #8192,4(%a4)
        bne.s   close
        tst.w   14(%a3)
        bne.s   close
        lea     256(%a3),%a0
        move.w  (%a0),%d0
        move.w  %d0,%d1
        move.w  #4095,%d2
1:      move.w  (%a0)+,%d3
        cmp.w   %d0,%d3
        bge.s   2f
        move.w  %d3,%d0
2:      cmp.w   %d1,%d3
        ble.s   3f
        move.w  %d3,%d1
3:      dbra    %d2,1b
        move.w  %d0,32(%a3)
        move.w  %d1,34(%a3)
close:
        move.w  #4,4(%a3)
        clr.w   -(%sp)
        move.l  (%a4),-(%sp)
        move.l  #0x021c0014,%d0
        .short  0xa800
        move.w  (%sp)+,28(%a3)
        move.w  #5,4(%a3)
done:   .short  0xa9f4
set_info:
        clr.w   -(%sp)
        move.l  (%a4),-(%sp)
        move.l  %d1,-(%sp)
        pea     16(%a3)
        move.l  #0x063c0014,%d0
        .short  0xa800
        move.w  (%sp)+,%d0
        rts
empty:  .word   0
