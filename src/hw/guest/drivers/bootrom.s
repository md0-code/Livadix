| Livadix — the built-in boot ROM's code (docs/NOTES.md, "The Built-in
| machine").
|
| Livadix's own boot firmware for the Macintosh 128K's board: it shows a desktop
| with a document icon for each of Livadix's documents (the README and the
| changelog); a double-click opens one in a window that scrolls (its arrows,
| its gray track, its thumb) and closes (its close box). It runs no Mac OS
| and reads no disk. tools/bootrom.py appends the data to this code (the
| fonts, the icon and the cursor, the documents wrapped into lines, the
| version) and fills the table at offset $10 with their addresses, makes
| the ROM a whole power of two and sets its checksum (offset 0) to 'LVDX'
| ($4C564458, mc_model "builtin").
|
| The hardware, as the Guide to the Macintosh Family Hardware, 2nd ed.,
| describes it (chapter 3, the address map: ROM at $40 0000 and at 0 under
| the overlay until VIA PA4 clears it, the RAM from 0, the SCC read at
| $9F FFF8 and written at $BF FFF9 with A1 the channel and A2 data or
| control, the VIA at $EF E1FE with its registers every $200; chapter 4,
| VIA port A (PA6 the main screen buffer, PA4 the overlay, PA3 the main
| sound buffer) and port B (PB7 the sound off, PB5/PB4 the mouse's Y2/X2,
| PB3 its button, PB2-PB0 the clock chip), CA1 the vertical blanking, the
| VIA on interrupt level 1, the SCC on level 2; chapter 12, the main screen
| buffer $5900 below the top of RAM, 512 x 342 one-bit pixels, 64 bytes a
| row; chapter 14, the mouse: X1 and Y1 on the SCC's DCDA and DCDB, which
| interrupt as they change, X2 and Y2 read on the VIA to tell the
| direction) and the Z8530 SCC's manual (WR9 reset and MIE, WR15 the DCD
| interrupt enable, WR1 the external/status enable, WR0's Reset
| External/Status Interrupts command). The mouse's sense: right when DCDA
| differs from X2, down when DCDB equals Y2.
|
| Assembler: GNU as for m68k (binutils), -m68000; built by
| tools/guest_build.py into src/hw/guest/bin/bootrom.bin.

        .set    ROM, 0x400000
        .set    H_FONT_BODY, ROM + 0x10 | the table tools/bootrom.py fills
        .set    H_FONT_UI, ROM + 0x14
        .set    H_DOCS, ROM + 0x18      | word count, word 0, then 12 bytes a document
        .set    H_ICON, ROM + 0x1C      | 32 rows of data, 32 of mask (longs)
        .set    H_VERSION, ROM + 0x20   | C string
        .set    H_CURSOR, ROM + 0x24    | 16 data/mask words, hotspot v/h words

        .set    SCREEN, 0x1A700         | 128 KB - $5900
        .set    STACK, 0x1A700
        .set    GLOB, 0x1000
| Where Livadix's shell places the pointer (machine.c, mc_machine_mouse_to:
| a Point, v then h, and a flag set from a byte it reads), as on Apple's
| ROMs: taken from there when the flag is set.
        .set    PLACED, 0x82C
        .set    PLACE_NEW, 0x8CE
        .set    PLACE_COUPLE, 0x8CF

        .set    VIA, 0xEFE1FE
        .set    vBufB, VIA
        .set    vDirB, VIA + 0x400
        .set    vDirA, VIA + 0x600
        .set    vACR, VIA + 0x1600
        .set    vPCR, VIA + 0x1800
        .set    vIFR, VIA + 0x1A00
        .set    vIER, VIA + 0x1C00
        .set    vBufA, VIA + 0x1E00
        .set    SCC_RB, 0x9FFFF8        | channel B control, read
        .set    SCC_RA, 0x9FFFFA        | channel A control, read
        .set    SCC_WB, 0xBFFFF9        | channel B control, write
        .set    SCC_WA, 0xBFFFFB        | channel A control, write

| Globals (from A5 = GLOB).
        .set    G_TICKS, 0              | long: vertical blankings
        .set    G_MX, 4                 | word: the mouse (the SCC's handler moves it)
        .set    G_MY, 6
        .set    G_DCDA, 8               | byte: DCDA as last seen
        .set    G_DCDB, 9
        .set    G_CURX, 10              | word: where the cursor is drawn
        .set    G_CURY, 12
        .set    G_CURROWS, 14           | word: its rows and bytes on the screen
        .set    G_CURBYTES, 16
        .set    G_CURVIS, 18            | byte: drawn
        .set    G_BTN, 19               | byte: the button as last seen
        .set    G_SEL, 20               | word: the selected icon, -1 none
        .set    G_LASTCLK, 22           | long: the tick of the last click on an icon
        .set    G_LASTICON, 26          | word
        .set    G_DOC, 28               | word: the open document, -1 none
        .set    G_TOP, 30               | word: its first line shown
        .set    G_NLINES, 32            | word
        .set    G_LINES, 34             | long: its line table
        .set    G_MAXTOP, 38            | word
        .set    G_TRACK, 40             | word: what the button holds (T_*)
        .set    G_REPEAT, 42            | long: the tick of its next repeat
        .set    G_THUMB, 46             | word: the thumb's top row
        .set    G_DRAG, 48              | word: the mouse's row in the thumb
        .set    G_LASTTICK, 50          | long
        .set    G_INSIDE, 54            | byte: the close box is shown pressed
        .set    G_CURSAVE, 56           | 16 x 4 bytes under the cursor
        .set    G_CURDRAWX, 120          | clipped cursor origin, distinct from its hotspot
        .set    G_CURDRAWY, 122
        .set    G_CURLEFT, 124
        .set    G_CURTOP, 126
        .set    G_SIZE, 128

        .set    T_NONE, 0
        .set    T_UP, 1
        .set    T_DOWN, 2
        .set    T_PAGEUP, 3
        .set    T_PAGEDOWN, 4
        .set    T_THUMB, 5
        .set    T_CLOSE, 6

| The desktop: the menu bar, two icons at the right.
        .set    BAR, 20                 | the menu bar's rows (its line the last)
        .set    ICX, 456                | the icons' left (a whole byte)
        .set    ICY, 34                 | the first icon's top
        .set    ICSTEP, 64
        .set    ICMID, 472              | their centre
        .set    DBLCLICK, 32            | ticks between the clicks of a double-click

| The window.
        .set    WX, 6
        .set    WY, 26
        .set    WW, 420
        .set    WH, 310
        .set    SBX, WX + WW - 16       | the scroll bar's left line
        .set    CX, WX + 1              | the content
        .set    CY, WY + 19
        .set    CW, SBX - CX
        .set    CH, WH - 20
        .set    TEXTX, 16               | the text's left: a whole byte (bytes 2-50 hold it)
        .set    TEXTY, CY + 4
        .set    LH, 11                  | a line's rows
        .set    VIS, 26                 | lines shown
        .set    TBYTES, 49
        .set    UPY, WY + 18            | the up arrow's box (17 rows with its lines)
        .set    TY0, UPY + 17           | the track's first row
        .set    DOWNY, WY + WH - 17     | the down arrow's box
        .set    TRACKH, DOWNY - TY0
        .set    THUMBH, 16
        .set    RANGE, TRACKH - THUMBH
        .set    CLOSEX, WX + 8
        .set    CLOSEY, WY + 4

        .text
| The header: the checksum (and the 68000's first stack pointer), the reset
| PC, the signature, the data table.
        .long   0                       | checksum: tools/bootrom.py
        .long   ROM + 0x40              | reset PC: start
        .ascii  "LVDXBOOT"
        .long   0, 0, 0, 0, 0, 0        | H_FONT_BODY .. H_CURSOR
        .long   0, 0
        .org    0x40
start:
        move.w  #0x2700,%sr
        | The VIA's ports: outputs set before their directions; the overlay
        | still on, then off (the RAM at 0).
        move.b  #0x58,vBufA             | PA6 main screen, PA4 overlay, PA3 main sound, volume 0
        move.b  #0x7F,vDirA             | PA7 (the SCC's /W//REQ) an input
        move.b  #0x84,vBufB             | PB7 sound off, PB2 the clock chip disabled
        move.b  #0x87,vDirB
        move.b  #0x48,vBufA             | the overlay off
        | The RAM cleared (128 KB).
        lea     0,%a0
        move.w  #0x8000-1,%d0
        moveq   #0,%d1
1:      move.l  %d1,(%a0)+
        dbra    %d0,1b
        lea     STACK,%sp
        lea     GLOB,%a5
        | The vectors: errors stop, interrupts return; level 1 the VIA, level 2 the SCC.
        lea     crash(%pc),%a1
        lea     8,%a0
        moveq   #12-2-1,%d0
2:      move.l  %a1,(%a0)+
        dbra    %d0,2b
        lea     ignore(%pc),%a1
        move.w  #(256-12)-1,%d0
3:      move.l  %a1,(%a0)+
        dbra    %d0,3b
        lea     via_int(%pc),%a1
        move.l  %a1,0x64
        lea     scc_int(%pc),%a1
        move.l  %a1,0x68
        | The VIA: the vertical blanking's interrupt alone (CA1, its falling edge).
        move.b  #0x7F,vIER
        move.b  #0x7F,vIFR
        move.b  #0x00,vACR
        move.b  #0x00,vPCR
        move.b  #0x82,vIER
        | The SCC: reset, then the DCD interrupts of both channels.
        lea     SCC_WB,%a0
        lea     SCC_WA,%a1
        move.b  #9,(%a0)
        move.b  #0xC0,(%a0)
        bsr     scc_channel
        exg     %a0,%a1
        bsr     scc_channel
        move.b  #9,(%a0)
        move.b  #0x08,(%a0)             | MIE
        move.b  SCC_RA,%d0
        btst    #3,%d0
        sne     G_DCDA(%a5)
        move.b  SCC_RB,%d0
        btst    #3,%d0
        sne     G_DCDB(%a5)
        | The state.
        move.w  #256,G_MX(%a5)
        move.w  #171,G_MY(%a5)
        move.w  #-1,G_SEL(%a5)
        move.w  #-1,G_DOC(%a5)
        move.w  #-1,G_LASTICON(%a5)
        st      PLACE_COUPLE
        move.w  #0x2000,%sr
        bsr     draw_desktop
        | The main loop: once a tick, the mouse and its button.
main:
        stop    #0x2000
        move.l  G_TICKS(%a5),%d0
        cmp.l   G_LASTTICK(%a5),%d0
        beq     main
        move.l  %d0,G_LASTTICK(%a5)
        bsr     read_mouse              | d6, d7
        btst    #3,vBufB
        seq     %d5                     | pressed
        move.b  G_BTN(%a5),%d4
        move.b  %d5,G_BTN(%a5)
        tst.b   %d5
        bne     1f
        tst.b   %d4
        beq     4f
        bsr     hide_cursor             | released
        bsr     mouse_up
        bra     4f
1:      bsr     hide_cursor
        tst.b   %d4
        bne     2f
        bsr     mouse_down
        bra     4f
2:      bsr     mouse_still
4:      bsr     update_cursor
        bra     main

scc_channel:
        move.b  #15,(%a0)
        move.b  #0x08,(%a0)             | WR15: DCD
        move.b  #1,(%a0)
        move.b  #0x01,(%a0)             | WR1: external/status interrupts
        move.b  #0x10,(%a0)
        move.b  #0x10,(%a0)
        rts

crash:  stop    #0x2700
        bra     crash
ignore: rte

| Level 1: the VIA (the vertical blanking counts a tick).
via_int:
        move.l  %d0,-(%sp)
        move.b  vIFR,%d0
        btst    #1,%d0
        beq     1f
        addq.l  #1,GLOB+G_TICKS
1:      move.b  #0x7F,vIFR
        move.l  (%sp)+,%d0
        rte

| Level 2: the SCC (the mouse moved a step).
scc_int:
        movem.l %d0-%d1/%a0,-(%sp)
        lea     GLOB,%a0
        move.b  SCC_RA,%d0              | X: DCDA against X2
        btst    #3,%d0
        sne     %d0
        cmp.b   G_DCDA(%a0),%d0
        beq     2f
        move.b  %d0,G_DCDA(%a0)
        move.b  vBufB,%d1
        btst    #4,%d1
        sne     %d1
        cmp.b   %d0,%d1
        beq     1f
        addq.w  #1,G_MX(%a0)
        bra     2f
1:      subq.w  #1,G_MX(%a0)
2:      move.b  #0x10,SCC_WA
        move.b  SCC_RB,%d0              | Y: DCDB against Y2
        btst    #3,%d0
        sne     %d0
        cmp.b   G_DCDB(%a0),%d0
        beq     4f
        move.b  %d0,G_DCDB(%a0)
        move.b  vBufB,%d1
        btst    #5,%d1
        sne     %d1
        cmp.b   %d0,%d1
        bne     3f
        addq.w  #1,G_MY(%a0)
        bra     4f
3:      subq.w  #1,G_MY(%a0)
4:      move.b  #0x10,SCC_WB
        movem.l (%sp)+,%d0-%d1/%a0
        rte

| The mouse kept on the screen: d6 x, d7 y.
read_mouse:
        move.w  #0x2700,%sr
        tst.b   PLACE_NEW
        beq     5f
        sf      PLACE_NEW
        move.w  PLACED,G_MY(%a5)
        move.w  PLACED+2,G_MX(%a5)
5:
        move.w  G_MX(%a5),%d6
        bpl     1f
        moveq   #0,%d6
1:      cmp.w   #511,%d6
        ble     2f
        move.w  #511,%d6
2:      move.w  %d6,G_MX(%a5)
        move.w  G_MY(%a5),%d7
        bpl     3f
        moveq   #0,%d7
3:      cmp.w   #341,%d7
        ble     4f
        move.w  #341,%d7
4:      move.w  %d7,G_MY(%a5)
        move.w  #0x2000,%sr
        rts

| --- the cursor ---------------------------------------------------------------------------

update_cursor:
        tst.b   G_CURVIS(%a5)
        beq     1f
        cmp.w   G_CURX(%a5),%d6
        bne     2f
        cmp.w   G_CURY(%a5),%d7
        beq     3f
2:      bsr     hide_cursor
1:      move.w  %d6,G_CURX(%a5)
        move.w  %d7,G_CURY(%a5)
        bsr     show_cursor
3:      rts

| The screen's byte at d0 x, d1 y in a0.
screen_at:
        move.l  %d2,-(%sp)
        moveq   #0,%d2
        move.w  %d1,%d2
        lsl.l   #6,%d2
        lea     SCREEN,%a0
        adda.l  %d2,%a0
        move.w  %d0,%d2
        lsr.w   #3,%d2
        adda.w  %d2,%a0
        move.l  (%sp)+,%d2
        rts

show_cursor:
        movem.l %d0-%d7/%a0-%a3,-(%sp)
        move.w  G_CURX(%a5),%d0
        move.w  G_CURY(%a5),%d1
        move.l  H_CURSOR,%a1
        sub.w   66(%a1),%d0            | the tip is the pointer's hotspot
        sub.w   64(%a1),%d1
        clr.w   G_CURLEFT(%a5)
        clr.w   G_CURTOP(%a5)
        tst.w   %d0
        bpl     8f
        move.w  #1,G_CURLEFT(%a5)
        clr.w   %d0
8:      tst.w   %d1
        bpl     9f
        move.w  #1,G_CURTOP(%a5)
        clr.w   %d1
9:      move.w  %d0,G_CURDRAWX(%a5)
        move.w  %d1,G_CURDRAWY(%a5)
        bsr     screen_at
        move.w  %d0,%d2
        lsr.w   #3,%d2
        moveq   #64,%d3
        sub.w   %d2,%d3                 | bytes left on the row
        cmp.w   #3,%d3
        ble     1f
        moveq   #3,%d3
1:      move.w  #342,%d4
        sub.w   %d1,%d4                 | rows left on the screen
        moveq   #16,%d2
        sub.w   G_CURTOP(%a5),%d2
        cmp.w   %d2,%d4
        ble     2f
        move.w  %d2,%d4
2:      move.w  %d4,G_CURROWS(%a5)
        move.w  %d3,G_CURBYTES(%a5)
        moveq   #7,%d5
        and.w   %d0,%d5
        tst.w   G_CURLEFT(%a5)
        beq     6f
        moveq   #-1,%d5
6:      move.l  H_CURSOR,%a1
        move.w  G_CURTOP(%a5),%d2
        add.w   %d2,%d2
        adda.w  %d2,%a1
        lea     G_CURSAVE(%a5),%a2
        subq.w  #1,%d4
3:      move.b  (%a0),(%a2)
        move.b  1(%a0),1(%a2)
        move.b  2(%a0),2(%a2)
        addq.l  #4,%a2
        moveq   #0,%d6
        move.w  32(%a1),%d6             | mask
        lsl.l   #8,%d6
        tst.w   %d5
        bmi     5f
        lsr.l   %d5,%d6
        bra     6f
5:      lsl.l   #1,%d6
6:      moveq   #0,%d7
        move.w  (%a1)+,%d7              | data
        lsl.l   #8,%d7
        tst.w   %d5
        bmi     5f
        lsr.l   %d5,%d7
        bra     6f
5:      lsl.l   #1,%d7
6:      move.l  %d6,%d0
        swap    %d0
        move.l  %d7,%d1
        swap    %d1
        lea     0(%a0),%a3
        bsr     cursor_byte
        cmp.w   #2,%d3
        blt     4f
        move.l  %d6,%d0
        lsr.l   #8,%d0
        move.l  %d7,%d1
        lsr.l   #8,%d1
        lea     1(%a0),%a3
        bsr     cursor_byte
        cmp.w   #3,%d3
        blt     4f
        move.b  %d6,%d0
        move.b  %d7,%d1
        lea     2(%a0),%a3
        bsr     cursor_byte
4:      lea     64(%a0),%a0
        dbra    %d4,3b
        st      G_CURVIS(%a5)
        movem.l (%sp)+,%d0-%d7/%a0-%a3
        rts
| (a3) = (a3) & ~d0 | d1 (low bytes).
cursor_byte:
        move.l  %d2,-(%sp)
        move.b  (%a3),%d2
        not.b   %d0
        and.b   %d0,%d2
        or.b    %d1,%d2
        move.b  %d2,(%a3)
        move.l  (%sp)+,%d2
        rts

hide_cursor:
        tst.b   G_CURVIS(%a5)
        beq     4f
        movem.l %d0-%d4/%a0-%a2,-(%sp)
        move.w  G_CURDRAWX(%a5),%d0
        move.w  G_CURDRAWY(%a5),%d1
        bsr     screen_at
        lea     G_CURSAVE(%a5),%a2
        move.w  G_CURROWS(%a5),%d4
        move.w  G_CURBYTES(%a5),%d3
        subq.w  #1,%d4
1:      move.b  (%a2),(%a0)
        cmp.w   #2,%d3
        blt     2f
        move.b  1(%a2),1(%a0)
        cmp.w   #3,%d3
        blt     2f
        move.b  2(%a2),2(%a0)
2:      addq.l  #4,%a2
        lea     64(%a0),%a0
        dbra    %d4,1b
        sf      G_CURVIS(%a5)
        movem.l (%sp)+,%d0-%d4/%a0-%a2
4:      rts

| --- drawing ----------------------------------------------------------------------------

| Fills d2 x d3 pixels at d0, d1 with the pattern at a2 (its byte for even
| rows, then odd ones).
fill_rect:
        movem.l %d0-%d7/%a0-%a1,-(%sp)
        tst.w   %d2
        ble     9f
        tst.w   %d3
        ble     9f
        bsr     masks                   | d2 left mask, d4 right mask, d6 bytes - 1, a0
        subq.w  #1,%d3
1:      move.w  %d1,%d7
        andi.w  #1,%d7
        move.b  0(%a2,%d7.w),%d7
        move.l  %a0,%a1
        tst.w   %d6
        bne     2f
        move.b  (%a1),%d5
        eor.b   %d7,%d5
        and.b   %d2,%d5
        eor.b   %d5,(%a1)
        bra     5f
2:      move.b  (%a1),%d5
        eor.b   %d7,%d5
        and.b   %d2,%d5
        eor.b   %d5,(%a1)+
        move.w  %d6,%d0
        subq.w  #2,%d0
        bmi     4f
3:      move.b  %d7,(%a1)+
        dbra    %d0,3b
4:      move.b  (%a1),%d5
        eor.b   %d7,%d5
        and.b   %d4,%d5
        eor.b   %d5,(%a1)
5:      lea     64(%a0),%a0
        addq.w  #1,%d1
        dbra    %d3,1b
9:      movem.l (%sp)+,%d0-%d7/%a0-%a1
        rts

| Inverts d2 x d3 pixels at d0, d1.
invert_rect:
        movem.l %d0-%d7/%a0-%a1,-(%sp)
        tst.w   %d2
        ble     9f
        tst.w   %d3
        ble     9f
        bsr     masks
        subq.w  #1,%d3
1:      move.l  %a0,%a1
        tst.w   %d6
        bne     2f
        eor.b   %d2,(%a1)
        bra     5f
2:      eor.b   %d2,(%a1)+
        move.w  %d6,%d0
        subq.w  #2,%d0
        bmi     4f
3:      not.b   (%a1)+
        dbra    %d0,3b
4:      eor.b   %d4,(%a1)
5:      lea     64(%a0),%a0
        dbra    %d3,1b
9:      movem.l (%sp)+,%d0-%d7/%a0-%a1
        rts

| For the rectangle d0, d1, d2 wide: d2 the first byte's mask, d4 the last
| one's, d6 the bytes it spans less one (with one byte, d2 both masks), a0
| its first byte.
masks:
        move.w  %d0,%d4
        add.w   %d2,%d4
        subq.w  #1,%d4                  | the last x
        move.w  %d0,%d5
        lsr.w   #3,%d5
        move.w  %d4,%d6
        lsr.w   #3,%d6
        sub.w   %d5,%d6
        moveq   #7,%d7
        and.w   %d0,%d7
        move.w  #0xFF,%d2
        lsr.w   %d7,%d2
        moveq   #7,%d7
        and.w   %d4,%d7
        eori.w  #7,%d7
        move.w  #0xFF,%d4
        lsl.w   %d7,%d4
        tst.w   %d6
        bne     1f
        and.b   %d4,%d2
1:      bra     screen_at

| Lines and frames in black.
hline:  | d0, d1, d2 wide
        movem.l %d3/%a2,-(%sp)
        moveq   #1,%d3
        lea     pat_black(%pc),%a2
        bsr     fill_rect
        movem.l (%sp)+,%d3/%a2
        rts
vline:  | d0, d1, d3 high
        movem.l %d2/%a2,-(%sp)
        moveq   #1,%d2
        lea     pat_black(%pc),%a2
        bsr     fill_rect
        movem.l (%sp)+,%d2/%a2
        rts
frame_rect:
        movem.l %d0-%d3,-(%sp)
        bsr     hline
        bsr     vline
        add.w   %d3,%d1
        subq.w  #1,%d1
        bsr     hline
        sub.w   %d3,%d1
        addq.w  #1,%d1
        add.w   %d2,%d0
        subq.w  #1,%d0
        bsr     vline
        movem.l (%sp)+,%d0-%d3
        rts
| A rectangle of the pattern at a2 (the caller's).
white_rect:
        move.l  %a2,-(%sp)
        lea     pat_white(%pc),%a2
        bsr     fill_rect
        move.l  (%sp)+,%a2
        rts
gray_rect:
        move.l  %a2,-(%sp)
        lea     pat_gray(%pc),%a2
        bsr     fill_rect
        move.l  (%sp)+,%a2
        rts

| A font (tools/bootrom.py): word height, word ascent, word first code,
| word count, a width byte each (to an even length), then each glyph's
| rows (words, bit 15 the leftmost pixel).
| Draws the character d2 at d0, d1 (its top row) in the font at a3; d0
| moves past it.
draw_char:
        movem.l %d1-%d7/%a0-%a1,-(%sp)
        bsr     glyph                   | d3 index, d4 width
        move.w  6(%a3),%d5
        addq.w  #1,%d5
        andi.w  #0xFFFE,%d5
        lea     8(%a3,%d5.w),%a1
        move.w  (%a3),%d6
        move.w  %d3,%d7
        mulu.w  %d6,%d7
        add.l   %d7,%d7
        adda.l  %d7,%a1
        bsr     screen_at
        moveq   #7,%d5
        and.w   %d0,%d5
        subq.w  #1,%d6
1:      moveq   #0,%d7
        move.w  (%a1)+,%d7
        beq     2f
        lsl.l   #8,%d7
        lsr.l   %d5,%d7
        or.b    %d7,2(%a0)
        lsr.l   #8,%d7
        or.b    %d7,1(%a0)
        lsr.w   #8,%d7
        or.b    %d7,(%a0)
2:      lea     64(%a0),%a0
        dbra    %d6,1b
        add.w   %d4,%d0
        addq.w  #1,%d0
        movem.l (%sp)+,%d1-%d7/%a0-%a1
        rts
| The glyph of d2 in the font at a3: d3 its index, d4 its width (a code the
| font lacks is drawn as '?').
glyph:
        moveq   #0,%d3
        move.b  %d2,%d3
        sub.w   4(%a3),%d3
        bmi     1f
        cmp.w   6(%a3),%d3
        blt     2f
1:      moveq   #63,%d3                 | '?'
        sub.w   4(%a3),%d3
2:      moveq   #0,%d4
        move.b  8(%a3,%d3.w),%d4
        rts

| The string at a1 at d0, d1 in the font at a3; d0 moves past it.
draw_string:
        movem.l %d2/%a1,-(%sp)
1:      moveq   #0,%d2
        move.b  (%a1)+,%d2
        beq     2f
        bsr     draw_char
        bra     1b
2:      movem.l (%sp)+,%d2/%a1
        rts
| The width of the string at a1 in the font at a3: d0.
text_width:
        movem.l %d2-%d4/%a1,-(%sp)
        moveq   #0,%d0
1:      moveq   #0,%d2
        move.b  (%a1)+,%d2
        beq     2f
        bsr     glyph
        add.w   %d4,%d0
        addq.w  #1,%d0
        bra     1b
2:      tst.w   %d0
        beq     3f
        subq.w  #1,%d0
3:      movem.l (%sp)+,%d2-%d4/%a1
        rts

| --- the desktop ------------------------------------------------------------------------

draw_desktop:
        movem.l %d0-%d5/%a1/%a3,-(%sp)
        moveq   #0,%d0
        moveq   #BAR,%d1
        move.w  #512,%d2
        move.w  #342-BAR,%d3
        bsr     gray_rect
        | The menu bar: Livadix and the version.
        moveq   #0,%d1
        move.w  #BAR-1,%d3
        bsr     white_rect
        move.w  #BAR-1,%d1
        bsr     hline
        move.l  H_FONT_UI,%a3
        moveq   #12,%d0
        moveq   #4,%d1
        lea     name(%pc),%a1
        bsr     draw_string
        move.l  H_FONT_BODY,%a3
        move.l  H_VERSION,%a1
        bsr     text_width
        neg.w   %d0
        add.w   #500,%d0
        moveq   #6,%d1
        bsr     draw_string
        | The icons, the window.
        moveq   #0,%d5
1:      bsr     draw_icon_n
        addq.w  #1,%d5
        move.l  H_DOCS,%a1
        cmp.w   (%a1),%d5
        blt     1b
        tst.w   G_DOC(%a5)
        bmi     2f
        bsr     draw_window
2:      movem.l (%sp)+,%d0-%d5/%a1/%a3
        rts

| The document d5's table entry in a1: long title, long line table, word lines.
doc_entry:
        move.l  %d0,-(%sp)
        move.l  H_DOCS,%a1
        move.w  %d5,%d0
        mulu.w  #12,%d0
        lea     4(%a1,%d0.w),%a1
        move.l  (%sp)+,%d0
        rts

| Icon d5 with its name, selected or not (G_SEL).
draw_icon_n:
        movem.l %d0-%d5/%a1/%a3,-(%sp)
        move.w  %d5,%d1
        mulu.w  #ICSTEP,%d1
        add.w   #ICY,%d1
        move.w  #ICMID-40,%d0
        moveq   #80,%d2
        moveq   #48,%d3
        bsr     gray_rect
        moveq   #0,%d4
        cmp.w   G_SEL(%a5),%d5
        bne     1f
        moveq   #1,%d4
1:      move.w  #ICX,%d0
        bsr     draw_icon
        bsr     doc_entry
        move.l  (%a1),%a1
        move.l  H_FONT_UI,%a3
        bsr     text_width
        move.w  %d0,%d2
        addq.w  #6,%d2                  | the name's box
        move.w  #ICMID,%d0
        move.w  %d2,%d3
        lsr.w   #1,%d3
        sub.w   %d3,%d0
        add.w   #33,%d1
        moveq   #14,%d3
        bsr     white_rect
        movem.l %d0-%d1,-(%sp)
        addq.w  #3,%d0
        addq.w  #1,%d1
        bsr     draw_string
        movem.l (%sp)+,%d0-%d1
        tst.w   %d4
        beq     2f
        bsr     invert_rect
2:      movem.l (%sp)+,%d0-%d5/%a1/%a3
        rts

| The icon at d0 (a whole byte), d1; d4 nonzero: selected (darkened).
draw_icon:
        movem.l %d0-%d7/%a0-%a3,-(%sp)
        bsr     screen_at
        move.l  H_ICON,%a1
        lea     128(%a1),%a2
        moveq   #32-1,%d7
1:      move.l  (%a1)+,%d2
        move.l  (%a2)+,%d3
        tst.w   %d4
        beq     2f
        eor.l   %d3,%d2
2:      moveq   #4-1,%d6
        move.l  %a0,%a3
3:      rol.l   #8,%d2
        rol.l   #8,%d3
        move.b  (%a3),%d5
        move.b  %d3,%d1
        not.b   %d1
        and.b   %d1,%d5
        or.b    %d2,%d5
        move.b  %d5,(%a3)+
        dbra    %d6,3b
        lea     64(%a0),%a0
        dbra    %d7,1b
        movem.l (%sp)+,%d0-%d7/%a0-%a3
        rts

| --- the window -------------------------------------------------------------------------

draw_window:
        movem.l %d0-%d5/%a1/%a2/%a3,-(%sp)
        moveq   #WX,%d0
        moveq   #WY,%d1
        move.w  #WW,%d2
        move.w  #WH,%d3
        bsr     white_rect
        bsr     frame_rect
        | Its shadow.
        addq.w  #1,%d0
        add.w   %d3,%d1
        bsr     hline
        move.w  #WX+WW,%d0
        moveq   #WY+1,%d1
        bsr     vline
        | The title bar: stripes, the close box, the title.
        moveq   #WX+2,%d0
        moveq   #WY+3,%d1
        move.w  #WW-4,%d2
        moveq   #13,%d3
        lea     pat_stripes(%pc),%a2
        bsr     fill_rect
        moveq   #CLOSEX-2,%d0
        moveq   #CLOSEY-1,%d1
        moveq   #15,%d2
        moveq   #13,%d3
        bsr     white_rect
        moveq   #CLOSEX,%d0
        moveq   #CLOSEY,%d1
        moveq   #11,%d2
        moveq   #11,%d3
        bsr     frame_rect
        move.w  G_DOC(%a5),%d5
        bsr     doc_entry
        move.l  (%a1),%a1
        move.l  H_FONT_UI,%a3
        bsr     text_width
        move.w  %d0,%d2
        add.w   #12,%d2
        move.w  #WX+WW/2,%d0
        move.w  %d2,%d3
        lsr.w   #1,%d3
        sub.w   %d3,%d0
        moveq   #WY+3,%d1
        moveq   #13,%d3
        bsr     white_rect
        addq.w  #6,%d0
        moveq   #WY+4,%d1
        bsr     draw_string
        moveq   #WX,%d0
        moveq   #WY+18,%d1
        move.w  #WW,%d2
        bsr     hline
        | The scroll bar: its line, the arrows' boxes, the track.
        move.w  #SBX,%d0
        moveq   #UPY,%d1
        move.w  #WH-18,%d3
        bsr     vline
        moveq   #16,%d2
        moveq   #17,%d3
        bsr     frame_rect
        move.w  #DOWNY,%d1
        bsr     frame_rect
        lea     arrow_up(%pc),%a1
        move.w  #UPY+5,%d1
        bsr     draw_arrow
        lea     arrow_down(%pc),%a1
        move.w  #DOWNY+5,%d1
        bsr     draw_arrow
        bsr     draw_track
        bsr     draw_text
        movem.l (%sp)+,%d0-%d5/%a1/%a2/%a3
        rts

| An arrow (7 rows of bytes, bit 7 left) in the scroll bar at row d1.
draw_arrow:
        movem.l %d0-%d3/%a1,-(%sp)
        moveq   #7-1,%d3
1:      move.b  (%a1)+,%d2
        move.w  #SBX+4,%d0
2:      lsl.b   #1,%d2
        bcc     3f
        movem.l %d2-%d3,-(%sp)
        moveq   #1,%d2
        moveq   #1,%d3
        move.l  %a2,-(%sp)
        lea     pat_black(%pc),%a2
        bsr     fill_rect
        move.l  (%sp)+,%a2
        movem.l (%sp)+,%d2-%d3
3:      addq.w  #1,%d0
        tst.b   %d2
        bne     2b
        addq.w  #1,%d1
        dbra    %d3,1b
        movem.l (%sp)+,%d0-%d3/%a1
        rts

| The track (gray when the document scrolls) and its thumb.
draw_track:
        movem.l %d0-%d3,-(%sp)
        move.w  #SBX+1,%d0
        move.w  #TY0,%d1
        moveq   #14,%d2
        move.w  #TRACKH,%d3
        tst.w   G_MAXTOP(%a5)
        beq     1f
        bsr     gray_rect
        bsr     thumb_row
        move.w  %d0,G_THUMB(%a5)
        bsr     draw_thumb
        bra     2f
1:      bsr     white_rect
2:      movem.l (%sp)+,%d0-%d3
        rts
| The thumb's top row for G_TOP: d0.
thumb_row:
        move.l  %d1,-(%sp)
        moveq   #0,%d0
        move.w  G_TOP(%a5),%d0
        mulu.w  #RANGE,%d0
        move.w  G_MAXTOP(%a5),%d1
        lsr.w   #1,%d1
        add.l   %d1,%d0
        divu.w  G_MAXTOP(%a5),%d0
        add.w   #TY0,%d0
        move.l  (%sp)+,%d1
        rts
| The thumb at G_THUMB.
draw_thumb:
        movem.l %d0-%d3,-(%sp)
        move.w  #SBX+1,%d0
        move.w  G_THUMB(%a5),%d1
        moveq   #14,%d2
        moveq   #THUMBH,%d3
        bsr     white_rect
        move.w  #SBX,%d0
        moveq   #16,%d2
        bsr     hline
        add.w   #THUMBH-1,%d1
        bsr     hline
        movem.l (%sp)+,%d0-%d3
        rts
| The thumb moved to d0.
move_thumb:
        movem.l %d0-%d3,-(%sp)
        cmp.w   G_THUMB(%a5),%d0
        beq     1f
        move.w  %d0,-(%sp)
        move.w  #SBX+1,%d0
        move.w  G_THUMB(%a5),%d1
        moveq   #14,%d2
        moveq   #THUMBH,%d3
        bsr     gray_rect
        move.w  (%sp)+,G_THUMB(%a5)
        bsr     draw_thumb
1:      movem.l (%sp)+,%d0-%d3
        rts

| The text from G_TOP.
draw_text:
        movem.l %d0-%d5,-(%sp)
        moveq   #CX,%d0
        moveq   #CY,%d1
        move.w  #CW,%d2
        move.w  #CH,%d3
        bsr     white_rect
        moveq   #0,%d5
1:      move.w  G_TOP(%a5),%d4
        add.w   %d5,%d4
        cmp.w   G_NLINES(%a5),%d4
        bge     2f
        bsr     draw_line
        addq.w  #1,%d5
        cmp.w   #VIS,%d5
        blt     1b
2:      movem.l (%sp)+,%d0-%d5
        rts
| Line d4 on row d5: its first byte 1 for a heading (underlined).
draw_line:
        movem.l %d0-%d3/%a1/%a3,-(%sp)
        move.w  %d5,%d1
        mulu.w  #LH,%d1
        add.w   #TEXTY,%d1
        move.l  G_LINES(%a5),%a1
        moveq   #0,%d0
        move.w  %d4,%d0
        lsl.l   #2,%d0
        move.l  0(%a1,%d0.l),%a1
        move.b  (%a1)+,%d3
        move.l  H_FONT_BODY,%a3
        moveq   #TEXTX,%d0
        bsr     draw_string
        tst.b   %d3
        beq     1f
        move.w  %d0,%d2
        sub.w   #TEXTX+1,%d2
        moveq   #TEXTX,%d0
        add.w   (%a3),%d1
        bsr     hline
1:      movem.l (%sp)+,%d0-%d3/%a1/%a3
        rts

| Scrolls to line d0 (kept in 0..G_MAXTOP): one line by moving the text
| and drawing the new line, more by drawing it all.
scroll_to:
        movem.l %d0-%d5/%a0-%a1,-(%sp)
        tst.w   %d0
        bpl     1f
        moveq   #0,%d0
1:      cmp.w   G_MAXTOP(%a5),%d0
        ble     2f
        move.w  G_MAXTOP(%a5),%d0
2:      move.w  G_TOP(%a5),%d1
        move.w  %d0,G_TOP(%a5)
        sub.w   %d1,%d0
        beq     9f
        cmp.w   #1,%d0
        beq     3f
        cmp.w   #-1,%d0
        beq     5f
        bsr     draw_text
        bra     8f
3:      | Down a line: the rows move up by LH.
        moveq   #TEXTX,%d0
        move.w  #TEXTY,%d1
        bsr     screen_at
        lea     LH*64(%a0),%a1
        move.w  #(VIS-1)*LH-1,%d2
4:      moveq   #TBYTES-1,%d3
        movem.l %a0-%a1,-(%sp)
41:     move.b  (%a1)+,(%a0)+
        dbra    %d3,41b
        movem.l (%sp)+,%a0-%a1
        lea     64(%a0),%a0
        lea     64(%a1),%a1
        dbra    %d2,4b
        moveq   #VIS-1,%d5
        bra     7f
5:      | Up a line: the rows move down by LH.
        moveq   #TEXTX,%d0
        move.w  #TEXTY+VIS*LH-1,%d1
        bsr     screen_at
        lea     -LH*64(%a0),%a1
        move.w  #(VIS-1)*LH-1,%d2
6:      moveq   #TBYTES-1,%d3
        movem.l %a0-%a1,-(%sp)
61:     move.b  (%a1)+,(%a0)+
        dbra    %d3,61b
        movem.l (%sp)+,%a0-%a1
        lea     -64(%a0),%a0
        lea     -64(%a1),%a1
        dbra    %d2,6b
        moveq   #0,%d5
7:      | The new line's rows cleared and drawn.
        moveq   #TEXTX,%d0
        move.w  %d5,%d1
        mulu.w  #LH,%d1
        add.w   #TEXTY,%d1
        move.w  #TBYTES*8,%d2
        moveq   #LH,%d3
        bsr     white_rect
        move.w  G_TOP(%a5),%d4
        add.w   %d5,%d4
        bsr     draw_line
8:      bsr     thumb_row
        bsr     move_thumb
9:      movem.l (%sp)+,%d0-%d5/%a0-%a1
        rts

| Opens document d5.
open_doc:
        movem.l %d0-%d1/%a1,-(%sp)
        move.w  %d5,G_DOC(%a5)
        bsr     doc_entry
        move.l  4(%a1),G_LINES(%a5)
        move.w  8(%a1),%d0
        move.w  %d0,G_NLINES(%a5)
        sub.w   #VIS,%d0
        bpl     1f
        moveq   #0,%d0
1:      move.w  %d0,G_MAXTOP(%a5)
        clr.w   G_TOP(%a5)
        bsr     draw_window
        movem.l (%sp)+,%d0-%d1/%a1
        rts

| --- the mouse --------------------------------------------------------------------------

| Whether d6, d7 is in the rectangle d0, d1, d2 x d3: d0 1 (bne) or 0
| (beq); d1-d3 kept.
in_rect:
        cmp.w   %d0,%d6
        blt     1f
        cmp.w   %d1,%d7
        blt     1f
        add.w   %d2,%d0
        cmp.w   %d0,%d6
        bge     1f
        move.w  %d1,%d0
        add.w   %d3,%d0
        cmp.w   %d0,%d7
        bge     1f
        moveq   #1,%d0
        rts
1:      moveq   #0,%d0
        rts

mouse_down:
        movem.l %d0-%d5,-(%sp)
        clr.w   G_TRACK(%a5)
        tst.w   G_DOC(%a5)
        bmi     icons
        | The window's close box and scroll bar.
        moveq   #CLOSEX,%d0
        moveq   #CLOSEY,%d1
        moveq   #11,%d2
        moveq   #11,%d3
        bsr     in_rect
        beq     1f
        move.w  #T_CLOSE,G_TRACK(%a5)
        bsr     press_close
        bra     done_down
1:      tst.w   G_MAXTOP(%a5)
        beq     icons
        move.w  #SBX,%d0
        moveq   #UPY,%d1
        moveq   #16,%d2
        moveq   #17,%d3
        bsr     in_rect
        beq     2f
        move.w  #T_UP,%d0
        bra     repeat_start
2:      move.w  #SBX,%d0
        move.w  #DOWNY,%d1
        bsr     in_rect
        beq     3f
        move.w  #T_DOWN,%d0
        bra     repeat_start
3:      move.w  #SBX,%d0
        move.w  #TY0,%d1
        move.w  #TRACKH,%d3
        bsr     in_rect
        beq     icons
        move.w  G_THUMB(%a5),%d1
        cmp.w   %d1,%d7
        bge     4f
        move.w  #T_PAGEUP,%d0
        bra     repeat_start
4:      add.w   #THUMBH,%d1
        cmp.w   %d1,%d7
        blt     5f
        move.w  #T_PAGEDOWN,%d0
        bra     repeat_start
5:      move.w  #T_THUMB,G_TRACK(%a5)
        move.w  %d7,%d0
        sub.w   G_THUMB(%a5),%d0
        move.w  %d0,G_DRAG(%a5)
        bra     done_down
repeat_start:
        move.w  %d0,G_TRACK(%a5)
        bsr     track_step
        move.l  G_TICKS(%a5),%d0
        add.l   #20,%d0
        move.l  %d0,G_REPEAT(%a5)
        bra     done_down
icons:  | An icon: selected; a second click on it soon after the first opens it.
        moveq   #0,%d5
6:      move.w  #ICMID-36,%d0
        move.w  %d5,%d1
        mulu.w  #ICSTEP,%d1
        add.w   #ICY,%d1
        moveq   #72,%d2
        moveq   #48,%d3
        bsr     in_rect
        bne     8f
        addq.w  #1,%d5
        move.l  H_DOCS,%a1
        cmp.w   (%a1),%d5
        blt     6b
        moveq   #-1,%d5                 | the desktop: nothing selected
        bsr     select
        bra     done_down
8:      bsr     select
        cmp.w   G_LASTICON(%a5),%d5
        bne     9f
        move.l  G_TICKS(%a5),%d0
        sub.l   G_LASTCLK(%a5),%d0
        cmp.l   #DBLCLICK,%d0
        bge     9f
        move.w  #-1,G_LASTICON(%a5)
        bsr     open_doc
        bra     done_down
9:      move.w  %d5,G_LASTICON(%a5)
        move.l  G_TICKS(%a5),G_LASTCLK(%a5)
done_down:
        movem.l (%sp)+,%d0-%d5
        rts

| Selects icon d5 (-1: none).
select:
        move.l  %d5,-(%sp)
        move.w  G_SEL(%a5),%d0
        cmp.w   %d0,%d5
        beq     1f
        move.w  %d5,G_SEL(%a5)
        move.w  %d0,%d5
        bmi     2f
        bsr     draw_icon_n             | the one selected before
2:      move.l  (%sp),%d5
        tst.w   %d5
        bmi     1f
        bsr     draw_icon_n
1:      move.l  (%sp)+,%d5
        rts

| One step of what the button holds in the scroll bar.
track_step:
        movem.l %d0-%d1,-(%sp)
        move.w  G_TOP(%a5),%d0
        move.w  G_TRACK(%a5),%d1
        cmp.w   #T_UP,%d1
        bne     1f
        subq.w  #1,%d0
        bra     8f
1:      cmp.w   #T_DOWN,%d1
        bne     2f
        addq.w  #1,%d0
        bra     8f
2:      | A page, while the thumb is not under the mouse yet.
        cmp.w   #T_PAGEUP,%d1
        bne     3f
        cmp.w   G_THUMB(%a5),%d7
        bge     9f
        sub.w   #VIS-1,%d0
        bra     8f
3:      move.w  G_THUMB(%a5),%d1
        add.w   #THUMBH,%d1
        cmp.w   %d1,%d7
        blt     9f
        add.w   #VIS-1,%d0
8:      bsr     scroll_to
9:      movem.l (%sp)+,%d0-%d1
        rts

mouse_still:
        movem.l %d0-%d4,-(%sp)
        move.w  G_TRACK(%a5),%d1
        beq     9f
        cmp.w   #T_CLOSE,%d1
        bne     1f
        bsr     press_close
        bra     9f
1:      cmp.w   #T_THUMB,%d1
        bne     2f
        | The thumb follows the mouse; the text follows the thumb.
        move.w  %d7,%d0
        sub.w   G_DRAG(%a5),%d0
        cmp.w   #TY0,%d0
        bge     11f
        move.w  #TY0,%d0
11:     cmp.w   #TY0+RANGE,%d0
        ble     12f
        move.w  #TY0+RANGE,%d0
12:     bsr     move_thumb
        sub.w   #TY0,%d0
        mulu.w  G_MAXTOP(%a5),%d0
        add.l   #RANGE/2,%d0
        divu.w  #RANGE,%d0
        cmp.w   G_TOP(%a5),%d0
        beq     9f
        move.w  %d0,G_TOP(%a5)
        bsr     draw_text
        bra     9f
2:      | The arrows and the pages repeat while the button stays down.
        move.l  G_TICKS(%a5),%d0
        cmp.l   G_REPEAT(%a5),%d0
        blt     9f
        addq.l  #3,%d0
        move.l  %d0,G_REPEAT(%a5)
        bsr     track_step
9:      movem.l (%sp)+,%d0-%d4
        rts

mouse_up:
        movem.l %d0-%d3,-(%sp)
        move.w  G_TRACK(%a5),%d1
        clr.w   G_TRACK(%a5)
        cmp.w   #T_THUMB,%d1
        bne     1f
        bsr     thumb_row               | the thumb settles on its line
        bsr     move_thumb
        bra     9f
1:      cmp.w   #T_CLOSE,%d1
        bne     9f
        tst.b   G_INSIDE(%a5)
        beq     9f
        bsr     invert_close
        move.w  #-1,G_DOC(%a5)
        bsr     draw_desktop
9:      movem.l (%sp)+,%d0-%d3
        rts

| The close box shown pressed while the mouse is in it.
press_close:
        movem.l %d0-%d3,-(%sp)
        moveq   #CLOSEX,%d0
        moveq   #CLOSEY,%d1
        moveq   #11,%d2
        moveq   #11,%d3
        bsr     in_rect
        sne     %d0
        cmp.b   G_INSIDE(%a5),%d0
        beq     1f
        bsr     invert_close
1:      movem.l (%sp)+,%d0-%d3
        rts
invert_close:
        movem.l %d0-%d3,-(%sp)
        not.b   G_INSIDE(%a5)
        moveq   #CLOSEX+1,%d0
        moveq   #CLOSEY+1,%d1
        moveq   #9,%d2
        moveq   #9,%d3
        bsr     invert_rect
        movem.l (%sp)+,%d0-%d3
        rts

| --- data -------------------------------------------------------------------------------

pat_white:      .byte   0x00, 0x00
pat_black:      .byte   0xFF, 0xFF
pat_gray:       .byte   0xAA, 0x55
pat_stripes:    .byte   0x00, 0xFF
arrow_up:       .byte   0x10, 0x38, 0x7C, 0xFE, 0x38, 0x38, 0x38
arrow_down:     .byte   0x38, 0x38, 0x38, 0xFE, 0x7C, 0x38, 0x10
name:           .asciz  "Livadix"
        .even
