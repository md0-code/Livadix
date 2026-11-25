/* Livadix guest serial check. Inside Macintosh: Devices, Serial Driver
 * and Device Manager. Open the printer/modem channel, clear hardware
 * handshaking (SerHShake), send PBXS followed by CR and read four
 * bytes. Use asynchronous calls and cancel after two guest seconds.
 *
 * The PPC boot probe leaves its cleared 128-byte record in A3.
 * Use its first free bytes for the payload, results and errors;
 * the parameter block and I/O buffer follow at offsets 64 and 120. */
        .text
        lea     64(%a3),%a4
        lea     aname(%pc),%a1
        move.l  %a1,18(%a4)
        move.l  %a4,%a0
        .short  0xa000
        move.w  %d0,52(%a3)
        bne.w   done
        move.w  24(%a4),%d6
        lea     iname(%pc),%a1
        move.l  %a1,18(%a4)
        move.l  %a4,%a0
        .short  0xa000
        move.w  %d0,54(%a3)
        bne.w   close_out
        move.w  24(%a4),%d7
        move.w  %d6,24(%a4)
        move.w  #10,26(%a4)
        clr.l   28(%a4)
        clr.l   32(%a4)
        move.l  %a4,%a0
        .short  0xa004
        clr.w   26(%a4)
        move.l  #0x50425853,120(%a3)
        move.b  #13,124(%a3)
        lea     120(%a3),%a1
        move.l  %a1,32(%a4)
        move.l  #5,36(%a4)
        move.l  %a4,%a0
        .short  0xa403
        bsr.s   wait
        move.w  16(%a4),48(%a3)
        move.w  42(%a4),50(%a3)
        tst.w   16(%a4)
        bne.s   cancel_out
        move.w  %d7,24(%a4)
        clr.l   120(%a3)
        move.l  #4,36(%a4)
        move.l  %a4,%a0
        .short  0xa402
        bsr.s   wait
        move.l  120(%a3),40(%a3)
        move.w  16(%a4),44(%a3)
        move.w  42(%a4),46(%a3)
        tst.w   16(%a4)
        ble.s   close_in
        move.l  %a4,%a0
        .short  0xa006
        bra.s   close_in
cancel_out:
        move.l  %a4,%a0
        .short  0xa006
close_in:
        move.w  %d7,24(%a4)
        move.l  %a4,%a0
        .short  0xa001
close_out:
        move.w  %d6,24(%a4)
        move.l  %a4,%a0
        .short  0xa001
        bra.s   done
wait:   move.l  0x16a,%d4
        add.l   #120,%d4
1:      .short  0xa9b4
        tst.w   16(%a4)
        ble.s   2f
        cmp.l   0x16a,%d4
        bhi.s   1b
2:      rts
aname:  .byte   5
        .ascii  ".AOut"
iname:  .byte   4
        .ascii  ".AIn"
        .even
done:
