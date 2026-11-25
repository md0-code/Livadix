# Livadix — livadixvideo_pci, the PowerPC native video driver ('ndrv', 'vido') of
# the PCI Livadix Display Card (the card's registers in
# src/hw/livadix_card.h). The same driver calls and the same rules as the 68k
# driver of the NuBus card (src/hw/guest/drivers/livadixvideo.s).
#
# Packaged by tools/pcirom.py into a PEF container (Mac OS Runtime
# Architectures, chapter 8): the code from code_base to code_end is section
# 0, everything from data_base to data_end section 1; the loader section
# resolves the import table at data_base (livadixcard_pci.yaml, same order as
# the IMP_ offsets below) and relocates the transition vectors. The card's
# FCode puts the container in the "driver,AAPL,MacOS,PowerPC" property,
# where Mac OS finds the driver of the display (Designing PCI Cards and
# Drivers for Power Macintosh Computers, chapters 7-9 and 11).
#
# Calling conventions (Mac OS Runtime Architectures, chapter 4, the
# PowerPC run-time): r1 the stack, its linkage area holding the LR at 8 and
# the TOC at 20; r2 the TOC, here the start of the data section, so every
# reference to the driver's data is r2-relative (LA); arguments in r3-r10,
# the result in r3; r13-r31 kept by every callee. DoDriverIO and the
# interrupt handler, the entries from outside, keep r24-r31 and set r31 to
# the driver's storage; cross-fragment calls go through the import's
# transition vector with the TOC saved at 20(r1) (CALL).
#
# DoDriverIO (Designing PCI Cards, chapter 8, "Native driver
# entry point"): r3 AddressSpaceID, r4 IOCommandID, r5 IOCommandContents,
# r6 IOCommandCode, r7 IOCommandKind. Initialize (and Replace) find the
# card through its Name Registry entry (the DriverInitInfo's RegEntryID):
# its "assigned-addresses" for the BARs and "AAPL,address" for their
# logical addresses, set the PCI command register's memory space bit
# (ExpMgrConfigReadWord/WriteWord), start the display in the preferred or
# configured mode and attach the card's vertical blank to the Video
# Services Library's VBL service ("driver-ist", InstallInterruptFunctions,
# VSLNewInterruptService). Control and Status take a CntrlParam (csCode at
# 26, csParam at 28, a pointer to the call's record), the records being
# those of the 68k driver (Designing Cards and Drivers for the Macintosh
# Family, 3rd ed., chapter 9; the Display Manager's driver interface).
# The preferred configuration is the device's "livadix,preferred" property,
# kept in NVRAM (RegistryPropertySetMod, kRegPropertyValueIsSavedToNVRAM).
#
# Assembler: GNU as for PowerPC (binutils), -mbig -mppc32 -mregnames; built
# by tools/guest_build.py into src/hw/guest/bin/livadixvideo_pci.bin, whose
# first 16 bytes give the code size, the data size and the offsets of the
# DoDriverIO transition vector and TheDriverDescription in the data.

        .set    controlErr, -17
        .set    statusErr, -18
        .set    paramErr, -50

        .set    R_VRAM, 0x008
        .set    R_MONITOR, 0x00C
        .set    R_MODES, 0x010
        .set    R_INIT_DEPTH, 0x018
        .set    R_WIDTH, 0x020
        .set    R_HEIGHT, 0x024
        .set    R_DEPTH, 0x028
        .set    R_STRIDE, 0x02C
        .set    R_BASE, 0x030
        .set    R_CONTROL, 0x040
        .set    R_STATUS, 0x044
        .set    R_CLUT_INDEX, 0x050
        .set    R_CLUT_DATA, 0x054
        .set    R_GAMMA_INDEX, 0x058
        .set    R_GAMMA_DATA, 0x05C
        .set    R_RES, 0x100
        .set    BLT_CAPS, 0x200
        .set    FB_BASE, 0x040          # the screen 64 bytes into VRAM, as on the NuBus card
        .set    FIRST_MODE, 128         # the 1-bit depth mode; 133 is 32 bits
        .set    FIRST_RES, 128          # the display mode ID of resolution 0

# IOCommandCode and IOCommandKind (Designing PCI Cards, chapter 8).
        .set    kOpenCommand, 0
        .set    kCloseCommand, 1
        .set    kControlCommand, 4
        .set    kStatusCommand, 5
        .set    kKillIOCommand, 6
        .set    kInitializeCommand, 7
        .set    kFinalizeCommand, 8
        .set    kReplaceCommand, 9
        .set    kSupersededCommand, 10
        .set    kImmediateIOCommandKind, 4

        .set    csCode, 26
        .set    csParam, 28
        .set    kRegPropertyValueIsSavedToNVRAM, 0x20

# Private storage (from r31).
        .set    S_BASE, 0               # long: the screen (VRAM + FB_BASE), logical
        .set    S_REGS, 4               # long: the registers, logical
        .set    S_DEVID, 8              # RegEntryID, 16 bytes
        .set    S_MODE, 24              # half: depth mode, 128-133
        .set    S_DEPTH, 26             # half: bits per pixel
        .set    S_RES, 28               # half: resolution index
        .set    S_WIDTH, 30             # half
        .set    S_HEIGHT, 32            # half
        .set    S_STRIDE, 34            # half
        .set    S_FLAGS, 36             # byte: bit 0 luminance mapping, 1 VBL off, 2 direct depth, 3 VBL service
        .set    S_MONITOR, 38           # half: the MONITOR register
        .set    S_VRAM, 40              # long: VRAM size
        .set    S_MODES, 44             # half: resolutions
        .set    S_GAMMA, 48             # gamma table: 12-byte header, three channels of 256
        .set    S_PREF, 828             # 8 bytes: depth mode, resolution + 1, MONITOR + 1
        .set    S_SVC, 836              # long: the VBL interrupt service
        .set    S_IST, 840              # 24 bytes: "driver-ist"
        .set    S_OLDREF, 864           # the interrupt member's refCon, enabler, disabler
        .set    S_OLDEN, 868
        .set    S_OLDDIS, 872
        .set    S_SIZE_W, 876           # long: a property size, in and out
        .set    S_ASSIGNED, 880         # 120 bytes: "assigned-addresses" (six entries of 20)
        .set    S_AADDR, 1000           # 24 bytes: "AAPL,address"
        .set    S_INITED, 1024          # long
        .set    S_PATCH, 1028           # long: the QuickDraw patch's container in the system heap
        .set    S_SIZE, 1032
        .set    F_GRAY, 1               # bit masks of S_FLAGS
        .set    F_VBLOFF, 2
        .set    F_DIRECT, 4
        .set    F_SERVICE, 8

# The import table (transition vector pointers), in livadixcard_pci.yaml's order.
        .set    IMP_InstallInterruptFunctions, 0
        .set    IMP_GetInterruptFunctions, 4
        .set    IMP_IOCommandIsComplete, 8
        .set    IMP_RegistryEntryIDCopy, 12
        .set    IMP_RegistryPropertyCreate, 16
        .set    IMP_RegistryPropertyGet, 20
        .set    IMP_RegistryPropertySet, 24
        .set    IMP_RegistryPropertySetMod, 28
        .set    IMP_VSLNewInterruptService, 32
        .set    IMP_VSLDisposeInterruptService, 36
        .set    IMP_VSLDoInterruptService, 40
        .set    IMP_ExpMgrConfigReadWord, 44
        .set    IMP_ExpMgrConfigWriteWord, 48
        .set    IMP_NewPtrSys, 52
        .set    IMP_MakeDataExecutable, 56
        .set    IMP_GetMemFragment, 60
        .set    IMPORTS, 16
        .set    kPrivateCFragCopy, 5

# --- macros -------------------------------------------------------------------

# rD = the address of a data symbol.
        .macro  LA rd, sym
        addi    \rd, r2, \sym - data_base
        .endm

# A frame of `size` bytes (a multiple of 16, at least 64: the linkage area,
# eight words of parameter area, locals from 56).
        .macro  ENTER size
        mflr    r0
        stw     r0, 8(r1)
        stwu    r1, -\size(r1)
        .endm
        .macro  LEAVE size
        addi    r1, r1, \size
        lwz     r0, 8(r1)
        mtlr    r0
        blr
        .endm

# A call through the import table entry `imp`.
        .macro  CALL imp
        lwz     r12, \imp(r2)
        stw     r2, 20(r1)
        lwz     r0, 0(r12)
        lwz     r2, 4(r12)
        mtctr   r0
        bctrl
        lwz     r2, 20(r1)
        .endm

# A call through the transition vector in rT (not r0).
        .macro  CALLTV rt
        mr      r12, \rt
        stw     r2, 20(r1)
        lwz     r0, 0(r12)
        lwz     r2, 4(r12)
        mtctr   r0
        bctrl
        lwz     r2, 20(r1)
        .endm

        .text

        .long   code_end - code_base
        .long   data_end - data_base
        .long   dodriverio_tvec - data_base
        .long   the_driver_description - data_base

code_base:

# --- the entry point ------------------------------------------------------------

DoDriverIO:
        ENTER   112
        stmw    r24, 80(r1)
        stw     r4, 56(r1)              # IOCommandID
        stw     r7, 60(r1)              # IOCommandKind
        LA      r31, storage
        mr      r30, r5
        cmpwi   r6, kInitializeCommand
        beq     io_init
        cmpwi   r6, kReplaceCommand
        beq     io_init
        cmpwi   r6, kFinalizeCommand
        beq     io_fini
        cmpwi   r6, kSupersededCommand
        beq     io_fini
        cmpwi   r6, kControlCommand
        beq     io_control
        cmpwi   r6, kStatusCommand
        beq     io_status
        cmpwi   r6, kOpenCommand
        beq     io_good
        cmpwi   r6, kCloseCommand
        beq     io_good
        cmpwi   r6, kKillIOCommand
        beq     io_good
        li      r3, paramErr
        b       io_done
io_good:
        li      r3, 0
        b       io_done
io_init:
        bl      initialize
        b       io_done
io_fini:
        bl      finalize
        b       io_done
io_control:
        bl      control
        b       io_done
io_status:
        bl      status
io_done:
        lwz     r0, 60(r1)
        andi.   r0, r0, kImmediateIOCommandKind
        bne     1f
        mr      r4, r3
        lwz     r3, 56(r1)
        CALL    IMP_IOCommandIsComplete
1:      lmw     r24, 80(r1)
        LEAVE   112

# --- initialize and finalize ----------------------------------------------------

# r30 = the DriverInitInfo (refNum, then the RegEntryID; Devices.h's 68k
# alignment puts it at 2). r3 = the result.
initialize:
        ENTER   64
        lwz     r0, S_INITED(r31)
        cmpwi   r0, 0
        bne     in_ok
        addi    r3, r30, 2
        addi    r4, r31, S_DEVID
        CALL    IMP_RegistryEntryIDCopy
        bl      find_card
        cmpwi   r3, 0
        beq     1f
        addi    r3, r30, 4              # in case of the PowerPC alignment
        addi    r4, r31, S_DEVID
        CALL    IMP_RegistryEntryIDCopy
        bl      find_card
        cmpwi   r3, 0
        bne     in_out
1:
        bl      memory_on
        lwz     r28, S_REGS(r31)
        lwz     r0, R_VRAM(r28)
        stw     r0, S_VRAM(r31)
        lwz     r0, R_MODES(r28)
        sth     r0, S_MODES(r31)
        lwz     r0, R_MONITOR(r28)
        sth     r0, S_MONITOR(r31)
        bl      pref_load
        bl      start_mode
        bl      gamma_linear
        bl      gamma_load
        bl      vbl_install
        bl      accel_install
        li      r0, 1
        stw     r0, S_INITED(r31)
in_ok:  li      r3, 0
in_out: LEAVE   64

finalize:
        ENTER   64
        lwz     r0, S_INITED(r31)
        cmpwi   r0, 0
        beq     1f
        bl      accel_remove
        bl      vbl_remove
        lwz     r28, S_REGS(r31)
        li      r0, 0
        stw     r0, R_CONTROL(r28)
        eieio
        stw     r0, S_INITED(r31)
1:      li      r3, 0
        LEAVE   64

# The card's BARs from "assigned-addresses" (phys.hi's register number $10
# the VRAM, $14 the registers), their logical addresses from the matching
# entries of "AAPL,address" when it is there (else the physical ones, which
# Mac OS maps one to one). r3 = 0, or -1 when either is missing.
find_card:
        ENTER   64
        li      r0, 0
        stw     r0, S_BASE(r31)
        stw     r0, S_REGS(r31)
        li      r0, 24
        stw     r0, S_SIZE_W(r31)
        addi    r3, r31, S_DEVID
        LA      r4, str_aapl_address
        addi    r5, r31, S_AADDR
        addi    r6, r31, S_SIZE_W
        CALL    IMP_RegistryPropertyGet
        cmpwi   r3, 0
        li      r27, 0                  # logical addresses known for this many entries
        bne     1f
        lwz     r27, S_SIZE_W(r31)
        srwi    r27, r27, 2
1:      li      r0, 120
        stw     r0, S_SIZE_W(r31)
        addi    r3, r31, S_DEVID
        LA      r4, str_assigned
        addi    r5, r31, S_ASSIGNED
        addi    r6, r31, S_SIZE_W
        CALL    IMP_RegistryPropertyGet
        cmpwi   r3, 0
        bne     fc_fail
        lwz     r26, S_SIZE_W(r31)      # bytes
        li      r25, 0                  # entry
        addi    r24, r31, S_ASSIGNED
2:      mulli   r0, r25, 20
        cmpw    r0, r26
        bge     fc_check
        lwz     r5, 0(r24)              # phys.hi
        lwz     r6, 8(r24)              # phys.lo
        cmpw    r25, r27
        bge     3f
        slwi    r0, r25, 2
        addi    r7, r31, S_AADDR
        lwzx    r6, r7, r0              # the logical address
3:      andi.   r5, r5, 0xFF
        cmpwi   r5, 0x10
        bne     4f
        addi    r6, r6, FB_BASE
        stw     r6, S_BASE(r31)
        b       5f
4:      cmpwi   r5, 0x14
        bne     5f
        stw     r6, S_REGS(r31)
5:      addi    r25, r25, 1
        addi    r24, r24, 20
        b       2b
fc_check:
        lwz     r0, S_BASE(r31)
        cmpwi   r0, 0
        beq     fc_fail
        lwz     r0, S_REGS(r31)
        cmpwi   r0, 0
        beq     fc_fail
        li      r3, 0
        LEAVE   64
fc_fail:
        li      r3, -1
        LEAVE   64

# The PCI command register's memory space bit (PCI Local Bus Specification
# 2.0, 6.2.2), which Open Firmware sets only when it opens the card.
memory_on:
        ENTER   64
        addi    r3, r31, S_DEVID
        li      r4, 4
        addi    r5, r1, 56
        CALL    IMP_ExpMgrConfigReadWord
        cmpwi   r3, 0
        bne     1f
        lhz     r5, 56(r1)
        andi.   r0, r5, 2
        bne     1f
        ori     r5, r5, 2
        addi    r3, r31, S_DEVID
        li      r4, 4
        CALL    IMP_ExpMgrConfigWriteWord
1:      LEAVE   64

# The mode the display starts in: the preferred configuration while the
# display is the one it was saved with and its depth fits, else the
# configured resolution (MONITOR) in the configured depth (INIT_DEPTH), or
# the deepest that fits below it.
start_mode:
        ENTER   64
        lbz     r3, S_PREF(r31)         # depth mode
        lbz     r4, S_PREF+1(r31)       # resolution + 1
        lbz     r5, S_PREF+2(r31)       # MONITOR + 1
        lhz     r11, S_MONITOR(r31)
        addi    r11, r11, 1
        cmpw    r5, r11
        bne     1f
        subi    r4, r4, 1
        lhz     r0, S_MODES(r31)
        cmplw   r4, r0
        bge     1f
        mr      r26, r3
        mr      r27, r4
        bl      depth_fits
        cmpwi   r3, 0
        beq     3f
1:      lhz     r27, S_MONITOR(r31)
        lwz     r28, S_REGS(r31)
        lwz     r3, R_INIT_DEPTH(r28)
        bl      mode_of
        mr      r26, r3
2:      mr      r3, r26
        mr      r4, r27
        bl      depth_fits
        cmpwi   r3, 0
        beq     3f
        subi    r26, r26, 1
        cmpwi   r26, FIRST_MODE
        bge     2b
        li      r26, FIRST_MODE
3:      mr      r3, r27
        bl      set_res
        mr      r3, r26
        bl      set_depth
        LEAVE   64

# --- the vertical blank ---------------------------------------------------------

# The card's interrupt member ("driver-ist", element 0) gets the driver's
# handler, keeping its enabler and disabler, which then enables it; the
# Video Services Library's VBL service runs the screen's VBL tasks.
vbl_install:
        ENTER   64
        li      r0, 24
        stw     r0, S_SIZE_W(r31)
        addi    r3, r31, S_DEVID
        LA      r4, str_driver_ist
        addi    r5, r31, S_IST
        addi    r6, r31, S_SIZE_W
        CALL    IMP_RegistryPropertyGet
        cmpwi   r3, 0
        bne     vi_none
        lwz     r3, S_IST(r31)
        lwz     r4, S_IST+4(r31)
        addi    r5, r31, S_OLDREF
        addi    r6, r1, 56              # the old handler: not kept
        addi    r7, r31, S_OLDEN
        addi    r8, r31, S_OLDDIS
        CALL    IMP_GetInterruptFunctions
        cmpwi   r3, 0
        bne     vi_none
        addi    r3, r31, S_DEVID
        lis     r4, 0x7662              # 'vbl '
        ori     r4, r4, 0x6C20
        addi    r5, r31, S_SVC
        CALL    IMP_VSLNewInterruptService
        cmpwi   r3, 0
        bne     vi_none
        lwz     r3, S_IST(r31)
        lwz     r4, S_IST+4(r31)
        mr      r5, r31
        LA      r6, vbl_isr_tvec
        li      r7, 0                   # the enabler and disabler stay
        li      r8, 0
        CALL    IMP_InstallInterruptFunctions
        cmpwi   r3, 0
        bne     vi_none
        lbz     r0, S_FLAGS(r31)
        ori     r0, r0, F_SERVICE
        stb     r0, S_FLAGS(r31)
        bl      vbl_on
        LEAVE   64
vi_none:
        lbz     r0, S_FLAGS(r31)
        ori     r0, r0, F_VBLOFF
        andi.   r0, r0, 0xFF - F_SERVICE
        stb     r0, S_FLAGS(r31)
        lwz     r28, S_REGS(r31)
        li      r0, 1                   # the display on, no interrupts
        stw     r0, R_CONTROL(r28)
        eieio
        LEAVE   64

vbl_remove:
        ENTER   64
        lbz     r0, S_FLAGS(r31)
        andi.   r0, r0, F_SERVICE
        beq     1f
        bl      vbl_off
        lwz     r3, S_SVC(r31)
        CALL    IMP_VSLDisposeInterruptService
        lbz     r0, S_FLAGS(r31)
        andi.   r0, r0, 0xFF - F_SERVICE
        stb     r0, S_FLAGS(r31)
1:      LEAVE   64

# The card's vertical blank interrupt on: the member enabled, CONTROL bit 1.
vbl_on:
        ENTER   64
        lwz     r28, S_REGS(r31)
        li      r0, 1
        stw     r0, R_STATUS(r28)
        li      r0, 3
        stw     r0, R_CONTROL(r28)
        eieio
        lwz     r9, S_OLDEN(r31)
        cmpwi   r9, 0
        beq     1f
        lwz     r3, S_IST(r31)
        lwz     r4, S_IST+4(r31)
        lwz     r5, S_OLDREF(r31)
        CALLTV  r9
1:      lbz     r0, S_FLAGS(r31)
        andi.   r0, r0, 0xFF - F_VBLOFF
        stb     r0, S_FLAGS(r31)
        LEAVE   64

vbl_off:
        ENTER   64
        lwz     r9, S_OLDDIS(r31)
        cmpwi   r9, 0
        beq     1f
        lwz     r3, S_IST(r31)
        lwz     r4, S_IST+4(r31)
        lwz     r5, S_OLDREF(r31)
        CALLTV  r9
1:      lwz     r28, S_REGS(r31)
        li      r0, 1
        stw     r0, R_CONTROL(r28)
        stw     r0, R_STATUS(r28)
        eieio
        lbz     r0, S_FLAGS(r31)
        ori     r0, r0, F_VBLOFF
        stb     r0, S_FLAGS(r31)
        LEAVE   64

# InterruptMemberNumber vbl_isr(InterruptSetMember (r3, r4), void *refCon
# (r5: the storage), UInt32 count): kIsrIsComplete (0) when the card's
# vertical blank was pending, else kIsrIsNotComplete (-1).
vbl_isr:
        ENTER   80
        stw     r31, 76(r1)
        stw     r28, 72(r1)
        mr      r31, r5
        lwz     r28, S_REGS(r31)
        lwz     r0, R_STATUS(r28)
        andi.   r0, r0, 1
        beq     1f
        li      r0, 1
        stw     r0, R_STATUS(r28)
        eieio
        lwz     r3, S_SVC(r31)
        CALL    IMP_VSLDoInterruptService
        li      r3, 0
        b       2f
1:      li      r3, -1
2:      lwz     r28, 72(r1)
        lwz     r31, 76(r1)
        LEAVE   80

# --- the preferred configuration ------------------------------------------------

pref_load:
        ENTER   64
        li      r0, 8
        stw     r0, S_SIZE_W(r31)
        addi    r3, r31, S_DEVID
        LA      r4, str_preferred
        addi    r5, r31, S_PREF
        addi    r6, r31, S_SIZE_W
        CALL    IMP_RegistryPropertyGet
        cmpwi   r3, 0
        beq     1f
        li      r0, 0
        stw     r0, S_PREF(r31)
        stw     r0, S_PREF+4(r31)
1:      LEAVE   64

# S_PREF into the device's property, kept in NVRAM. r3 = 0.
pref_save:
        ENTER   64
        lhz     r11, S_MONITOR(r31)
        addi    r11, r11, 1
        stb     r11, S_PREF+2(r31)
        addi    r3, r31, S_DEVID
        LA      r4, str_preferred
        addi    r5, r31, S_PREF
        li      r6, 8
        CALL    IMP_RegistryPropertySet
        cmpwi   r3, 0
        beq     1f
        addi    r3, r31, S_DEVID
        LA      r4, str_preferred
        addi    r5, r31, S_PREF
        li      r6, 8
        CALL    IMP_RegistryPropertyCreate
1:      addi    r3, r31, S_DEVID
        LA      r4, str_preferred
        li      r5, kRegPropertyValueIsSavedToNVRAM
        CALL    IMP_RegistryPropertySetMod
        li      r3, 0
        LEAVE   64

# --- the native QuickDraw patch ---------------------------------------------------

# The card's accelerated QuickDraw (livadixcard_accel_ppc.s, the code fragment
# carried at accel_pef) when the card has its blitter (BLT_CAPS bit 0,
# video.acceleration): the container copied into the system heap
# (NewPtrSys; MakeDataExecutable), prepared by the Code Fragment Manager
# (GetMemFragment, kPrivateCFragCopy; Inside Macintosh: PowerPC System
# Software, chapter 3) and its main symbol called, install(VRAM,
# registers). Once, whatever Initialize and Replace follow; it stays when
# the driver goes (a patch of a screen no longer the card's goes on to
# QuickDraw).
        .set    A_CONN, 56              # the frame: the connection
        .set    A_MAIN, 60              # the main symbol
        .set    A_ERR, 64               # the error's name, a Str255
accel_install:
        ENTER   336
        lwz     r0, S_PATCH(r31)
        cmpwi   r0, 0
        bne     9f
        lwz     r28, S_REGS(r31)
        lwz     r0, BLT_CAPS(r28)
        andi.   r0, r0, 1
        beq     9f
        li      r3, accel_pef_end - accel_pef
        CALL    IMP_NewPtrSys
        cmpwi   r3, 0
        beq     9f
        mr      r29, r3
        LA      r4, accel_pef - 4
        addi    r5, r3, -4
        li      r0, (accel_pef_end - accel_pef) / 4
        mtctr   r0
1:      lwzu    r0, 4(r4)
        stwu    r0, 4(r5)
        bdnz    1b
        mr      r3, r29
        li      r4, accel_pef_end - accel_pef
        CALL    IMP_MakeDataExecutable
        stw     r29, S_PATCH(r31)
        mr      r3, r29
        li      r4, accel_pef_end - accel_pef
        LA      r5, str_accel
        li      r6, kPrivateCFragCopy
        addi    r7, r1, A_CONN
        addi    r8, r1, A_MAIN
        addi    r9, r1, A_ERR
        CALL    IMP_GetMemFragment
        cmpwi   r3, 0
        bne     9f
        lwz     r3, S_BASE(r31)
        subi    r3, r3, FB_BASE         # the VRAM
        lwz     r4, S_REGS(r31)
        lwz     r11, A_MAIN(r1)
        CALLTV  r11
9:      LEAVE   336
accel_remove:
        blr

# --- control --------------------------------------------------------------------

# r30 = the CntrlParam. Handlers run with r29 = csParam, r28 = the
# registers; r3 = the result.
control:
        ENTER   64
        lwz     r29, csParam(r30)
        lwz     r28, S_REGS(r31)
        lha     r0, csCode(r30)
        cmpwi   r0, 0
        beq     ctl_reset
        cmpwi   r0, 1
        beq     ctl_good
        cmpwi   r0, 2
        beq     ctl_setmode
        cmpwi   r0, 3
        beq     ctl_setentries
        cmpwi   r0, 4
        beq     ctl_setgamma
        cmpwi   r0, 5
        beq     ctl_grayscreen
        cmpwi   r0, 6
        beq     ctl_setgray
        cmpwi   r0, 7
        beq     ctl_setinterrupt
        cmpwi   r0, 8
        beq     ctl_directsetentries
        cmpwi   r0, 9
        beq     ctl_setdefaultmode
        cmpwi   r0, 10
        beq     ctl_switchmode
        cmpwi   r0, 16
        beq     ctl_savepreferred
ctl_bad:
        li      r3, controlErr
        LEAVE   64
ctl_good:
        li      r3, 0
        LEAVE   64

# Reset: 1 bit, page 0.
ctl_reset:
        li      r3, FIRST_MODE
        bl      set_depth
        cmpwi   r3, 0
        bne     ctl_bad
        li      r0, FIRST_MODE
        sth     r0, 0(r29)              # csMode
        li      r0, 0
        sth     r0, 6(r29)              # csPage
        lwz     r0, S_BASE(r31)
        stw     r0, 8(r29)              # csBaseAddr
        b       ctl_good

# SetMode: csMode (a depth mode), csPage 0.
ctl_setmode:
        lhz     r0, 6(r29)
        cmpwi   r0, 0
        bne     ctl_bad
        lhz     r3, 0(r29)
        bl      set_depth
        cmpwi   r3, 0
        bne     ctl_bad
        lwz     r0, S_BASE(r31)
        stw     r0, 8(r29)
        b       ctl_good

ctl_setentries:
        lbz     r0, S_FLAGS(r31)
        andi.   r0, r0, F_DIRECT
        bne     ctl_bad
        bl      set_entries
        LEAVE   64

ctl_directsetentries:
        lbz     r0, S_FLAGS(r31)
        andi.   r0, r0, F_DIRECT
        beq     ctl_bad
        bl      set_entries
        LEAVE   64

# SetGamma: csGTable, a table of 1 or 3 channels of 256 bytes, NIL for a
# linear one.
ctl_setgamma:
        lwz     r3, 0(r29)
        cmpwi   r3, 0
        beq     6f
        lhz     r0, 0(r3)               # gVersion 0
        cmpwi   r0, 0
        bne     ctl_bad
        lhz     r0, 8(r3)               # gDataCnt
        cmpwi   r0, 256
        bne     ctl_bad
        lhz     r0, 10(r3)              # gDataWidth
        cmpwi   r0, 8
        bne     ctl_bad
        lhz     r5, 6(r3)               # gChanCnt
        cmpwi   r5, 1
        beq     1f
        cmpwi   r5, 3
        bne     ctl_bad
1:      lhz     r0, 4(r3)               # gFormulaSize
        add     r3, r3, r0
        addi    r3, r3, 12              # the data
        bl      gamma_header
        addi    r4, r31, S_GAMMA + 12
        li      r6, 0
        cmpwi   r5, 3
        beq     3f
2:      lbzx    r0, r3, r6              # one channel for all three
        stbx    r0, r4, r6
        addi    r7, r6, 256
        stbx    r0, r4, r7
        addi    r7, r6, 512
        stbx    r0, r4, r7
        addi    r6, r6, 1
        cmpwi   r6, 256
        blt     2b
        b       7f
3:      lbzx    r0, r3, r6
        stbx    r0, r4, r6
        addi    r6, r6, 1
        cmpwi   r6, 768
        blt     3b
        b       7f
6:      bl      gamma_linear
7:      bl      gamma_load
        b       ctl_good

# GrayScreen: csPage 0 filled with the depth's 50% dither; in a direct depth
# the CLUT gets its linear ramps.
ctl_grayscreen:
        lhz     r0, 6(r29)
        cmpwi   r0, 0
        bne     ctl_bad
        bl      gray_fill
        lbz     r0, S_FLAGS(r31)
        andi.   r0, r0, F_DIRECT
        beq     ctl_good
        bl      clut_ramp
        b       ctl_good

# SetGray: csMode 1 maps SetEntries colours to their luminance.
ctl_setgray:
        lbz     r0, S_FLAGS(r31)
        andi.   r0, r0, 0xFF - F_GRAY
        lbz     r3, 0(r29)
        cmpwi   r3, 0
        beq     1f
        ori     r0, r0, F_GRAY
1:      stb     r0, S_FLAGS(r31)
        b       ctl_good

# SetInterrupt: csMode 0 enables the VBL interrupt, 1 disables it.
ctl_setinterrupt:
        lbz     r0, S_FLAGS(r31)
        andi.   r4, r0, F_SERVICE
        beq     ctl_good                # no interrupt to switch
        andi.   r4, r0, F_VBLOFF
        lbz     r3, 0(r29)
        cmpwi   r3, 0
        bne     1f
        cmpwi   r4, 0
        beq     ctl_good
        bl      vbl_on
        b       ctl_good
1:      cmpwi   r4, 0
        bne     ctl_good
        bl      vbl_off
        b       ctl_good

# SetDefaultMode: the display mode ID (csMode, a byte) to start in.
ctl_setdefaultmode:
        lbz     r4, 0(r29)
        subi    r4, r4, FIRST_RES
        lhz     r0, S_MODES(r31)
        cmplw   r4, r0
        bge     ctl_bad
        addi    r4, r4, 1
        stb     r4, S_PREF+1(r31)
        lbz     r0, S_PREF(r31)
        cmpwi   r0, 0
        bne     1f
        lhz     r0, S_MODE(r31)
        stb     r0, S_PREF(r31)
1:      bl      pref_save
        b       ctl_good

# SavePreferredConfiguration: csMode (depth mode) and csData (display mode
# ID) of a VDSwitchInfoRec.
ctl_savepreferred:
        lwz     r4, 2(r29)
        subi    r4, r4, FIRST_RES
        lhz     r0, S_MODES(r31)
        cmplw   r4, r0
        bge     ctl_bad
        lhz     r3, 0(r29)
        mr      r27, r4
        bl      depth_fits
        cmpwi   r3, 0
        bne     ctl_bad
        lhz     r0, 0(r29)
        stb     r0, S_PREF(r31)
        addi    r0, r27, 1
        stb     r0, S_PREF+1(r31)
        bl      pref_save
        b       ctl_good

# SwitchMode: csMode (depth mode), csData (display mode ID), csPage 0.
ctl_switchmode:
        lhz     r0, 6(r29)
        cmpwi   r0, 0
        bne     ctl_bad
        lwz     r4, 2(r29)
        subi    r4, r4, FIRST_RES
        lhz     r0, S_MODES(r31)
        cmplw   r4, r0
        bge     ctl_bad
        lhz     r3, 0(r29)
        mr      r27, r4
        bl      depth_fits
        cmpwi   r3, 0
        bne     ctl_bad
        mr      r3, r27
        bl      set_res
        lhz     r3, 0(r29)
        bl      set_depth
        cmpwi   r3, 0
        bne     ctl_bad
        lwz     r0, S_BASE(r31)
        stw     r0, 8(r29)
        b       ctl_good

# --- status ---------------------------------------------------------------------

status:
        ENTER   64
        lwz     r29, csParam(r30)
        lwz     r28, S_REGS(r31)
        lha     r0, csCode(r30)
        cmpwi   r0, 2
        beq     st_getmode
        cmpwi   r0, 3
        beq     st_getentries
        cmpwi   r0, 4
        beq     st_getpages
        cmpwi   r0, 5
        beq     st_getbaseaddr
        cmpwi   r0, 6
        beq     st_getgray
        cmpwi   r0, 7
        beq     st_getinterrupt
        cmpwi   r0, 8
        beq     st_getgamma
        cmpwi   r0, 9
        beq     st_getdefaultmode
        cmpwi   r0, 10
        beq     st_getcurmode
        cmpwi   r0, 12
        beq     st_getconnection
        cmpwi   r0, 13
        beq     st_getmodetiming
        cmpwi   r0, 16
        beq     st_getpreferred
        cmpwi   r0, 17
        beq     st_getnextresolution
        cmpwi   r0, 18
        beq     st_getvideoparameters
st_bad:
        li      r3, statusErr
        LEAVE   64
st_good:
        li      r3, 0
        LEAVE   64

st_getmode:
        lhz     r0, S_MODE(r31)
        sth     r0, 0(r29)
        li      r0, 0
        sth     r0, 6(r29)
        lwz     r0, S_BASE(r31)
        stw     r0, 8(r29)
        b       st_good

# GetEntries: csTable, csStart (-1: by the value fields), csCount (zero
# based).
st_getentries:
        lwz     r4, 0(r29)              # csTable
        lha     r5, 4(r29)              # csStart
        lha     r6, 6(r29)              # csCount
        cmpwi   r6, 0
        blt     st_bad
1:      cmpwi   r5, 0
        blt     2f
        sth     r5, 0(r4)
        mr      r0, r5
        addi    r5, r5, 1
        b       3f
2:      lhz     r0, 0(r4)
3:      andi.   r0, r0, 0xFF
        stw     r0, R_CLUT_INDEX(r28)
        eieio
        li      r7, 3
        addi    r8, r4, 2
4:      lwz     r0, R_CLUT_DATA(r28)
        stb     r0, 0(r8)
        stb     r0, 1(r8)
        addi    r8, r8, 2
        subic.  r7, r7, 1
        bne     4b
        addi    r4, r4, 8
        subic.  r6, r6, 1
        bge     1b
        b       st_good

st_getpages:
        li      r0, 1
        sth     r0, 6(r29)
        b       st_good

st_getbaseaddr:
        lhz     r0, 6(r29)
        cmpwi   r0, 0
        bne     st_bad
        lwz     r0, S_BASE(r31)
        stw     r0, 8(r29)
        b       st_good

st_getgray:
        lbz     r0, S_FLAGS(r31)
        andi.   r0, r0, F_GRAY
        beq     1f
        li      r0, 1
1:      stb     r0, 0(r29)
        b       st_good

st_getinterrupt:
        lbz     r0, S_FLAGS(r31)
        andi.   r0, r0, F_VBLOFF
        beq     1f
        li      r0, 1
1:      stb     r0, 0(r29)
        b       st_good

st_getgamma:
        addi    r0, r31, S_GAMMA
        stw     r0, 0(r29)
        b       st_good

# GetDefaultMode: the display mode ID SetDefaultMode kept, else the current one.
st_getdefaultmode:
        lbz     r11, S_PREF+1(r31)
        cmpwi   r11, 0
        beq     1f
        addi    r11, r11, FIRST_RES - 1
        b       2f
1:      lhz     r11, S_RES(r31)
        addi    r11, r11, FIRST_RES
2:      stb     r11, 0(r29)
        b       st_good

# GetCurMode: csMode, csData (display mode ID), csPage, csBaseAddr.
st_getcurmode:
        lhz     r0, S_MODE(r31)
        sth     r0, 0(r29)
        lhz     r11, S_RES(r31)
        addi    r11, r11, FIRST_RES
        stw     r11, 2(r29)
        li      r0, 0
        sth     r0, 6(r29)
        lwz     r0, S_BASE(r31)
        stw     r0, 8(r29)
        b       st_good

# GetConnection: kMultiModeCRT3Connect (6), kAllModesValid | kAllModesSafe.
st_getconnection:
        li      r0, 6
        sth     r0, 0(r29)              # csDisplayType
        li      r0, 0
        sth     r0, 2(r29)              # csConnectTaggedType, csConnectTaggedData
        stw     r0, 8(r29)              # csDisplayComponent
        stw     r0, 12(r29)
        li      r0, 3
        stw     r0, 4(r29)              # csConnectFlags
        b       st_good

# GetModeTiming: csTimingMode in; csTimingFormat 'decl', csTimingData
# timingInvalid, csTimingFlags kModeValid | kModeSafe, with kModeDefault for
# the display's own resolution.
st_getmodetiming:
        lwz     r4, 0(r29)
        subi    r4, r4, FIRST_RES
        lhz     r0, S_MODES(r31)
        cmplw   r4, r0
        bge     st_bad
        lis     r0, 0x6465              # 'decl'
        ori     r0, r0, 0x636C
        stw     r0, 8(r29)
        li      r0, 0
        stw     r0, 12(r29)
        li      r0, 3
        lhz     r5, S_MONITOR(r31)
        cmpw    r4, r5
        bne     1f
        li      r0, 7
1:      stw     r0, 16(r29)
        b       st_good

# GetPreferredConfiguration: the depth and resolution kept, else the
# current ones.
st_getpreferred:
        lhz     r0, S_MODE(r31)
        lbz     r3, S_PREF(r31)
        cmpwi   r3, FIRST_MODE
        blt     1f
        cmpwi   r3, FIRST_MODE + 5
        bgt     1f
        mr      r0, r3
1:      sth     r0, 0(r29)
        lhz     r11, S_RES(r31)
        lbz     r3, S_PREF+1(r31)
        subi    r3, r3, 1
        lhz     r4, S_MODES(r31)
        cmplw   r3, r4
        bge     2f
        mr      r11, r3
2:      addi    r11, r11, FIRST_RES
        stw     r11, 2(r29)
        li      r0, 0
        sth     r0, 6(r29)
        lwz     r0, S_BASE(r31)
        stw     r0, 8(r29)
        b       st_good

# GetNextResolution: csPreviousDisplayModeID in (-2 the first, 0 the
# current); csDisplayModeID (-3 after the last), csHorizontalPixels,
# csVerticalLines, csRefreshRate, csMaxDepthMode out.
st_getnextresolution:
        lwz     r4, 0(r29)
        lhz     r6, S_MODES(r31)
        cmpwi   r4, -2
        bne     1f
        li      r27, 0
        b       3f
1:      cmpwi   r4, 0
        bne     2f
        lhz     r27, S_RES(r31)
        b       3f
2:      subi    r4, r4, FIRST_RES
        cmplw   r4, r6
        bge     st_bad
        addi    r27, r4, 1
3:      cmplw   r27, r6
        blt     4f
        li      r0, -3
        stw     r0, 4(r29)
        b       st_good
4:      addi    r0, r27, FIRST_RES
        stw     r0, 4(r29)
        mr      r3, r27
        bl      res_entry
        srwi    r0, r3, 16
        stw     r0, 8(r29)              # csHorizontalPixels
        andi.   r0, r3, 0xFFFF
        stw     r0, 12(r29)             # csVerticalLines
        lis     r0, 60                  # csRefreshRate: 60 Hz, Fixed
        stw     r0, 16(r29)
        li      r26, FIRST_MODE + 5     # the deepest mode that fits the VRAM
5:      mr      r3, r26
        mr      r4, r27
        bl      depth_fits
        cmpwi   r3, 0
        beq     6f
        subi    r26, r26, 1
        cmpwi   r26, FIRST_MODE
        bgt     5b
6:      sth     r26, 20(r29)            # csMaxDepthMode
        li      r0, 0
        stw     r0, 22(r29)             # csResolutionFlags
        stw     r0, 26(r29)
        b       st_good

# GetVideoParameters: csDisplayModeID (0 the current), csDepthMode in; the
# VPBlock at csVPBlockPtr, csPageCount, csDeviceType out.
st_getvideoparameters:
        lwz     r4, 0(r29)
        cmpwi   r4, 0
        bne     1f
        lhz     r27, S_RES(r31)
        b       2f
1:      subi    r27, r4, FIRST_RES
        lhz     r0, S_MODES(r31)
        cmplw   r27, r0
        bge     st_bad
2:      lhz     r26, 4(r29)
        mr      r3, r26
        mr      r4, r27
        bl      depth_fits
        cmpwi   r3, 0
        bne     st_bad
        mr      r3, r27
        bl      res_entry
        mr      r25, r3                 # width << 16 | height
        srwi    r3, r25, 16
        mr      r4, r26
        bl      stride_of
        lwz     r8, 6(r29)              # the VPBlock
        li      r0, 0
        stw     r0, 0(r8)               # vpBaseOffset
        sth     r3, 4(r8)               # vpRowBytes
        sth     r0, 6(r8)               # vpBounds: top, left
        sth     r0, 8(r8)
        sth     r25, 10(r8)             # bottom
        srwi    r0, r25, 16
        sth     r0, 12(r8)              # right
        li      r0, 0
        sth     r0, 14(r8)              # vpVersion
        sth     r0, 16(r8)              # vpPackType
        stw     r0, 18(r8)              # vpPackSize
        lis     r0, 0x48                # vpHRes, vpVRes: 72 dpi
        stw     r0, 22(r8)
        stw     r0, 26(r8)
        subi    r5, r26, FIRST_MODE
        li      r6, 1
        slw     r6, r6, r5              # bits
        cmpwi   r6, 16
        blt     3f
        li      r0, 16
        sth     r0, 30(r8)              # vpPixelType: ChunkyDirect
        sth     r6, 32(r8)              # vpPixelSize
        li      r0, 3
        sth     r0, 34(r8)              # vpCmpCount
        li      r0, 5
        cmpwi   r6, 16
        beq     4f
        li      r0, 8
4:      sth     r0, 36(r8)              # vpCmpSize
        li      r7, 2                   # directType
        b       5f
3:      li      r0, 0
        sth     r0, 30(r8)              # ChunkyIndexed
        sth     r6, 32(r8)
        li      r0, 1
        sth     r0, 34(r8)
        sth     r6, 36(r8)
        li      r7, 0                   # clutType
5:      li      r0, 0
        stw     r0, 38(r8)              # vpPlaneBytes
        li      r0, 1
        stw     r0, 10(r29)             # csPageCount
        stw     r7, 14(r29)             # csDeviceType
        b       st_good

# --- helpers --------------------------------------------------------------------

# r3 = RES[r3] (width << 16 | height).
res_entry:
        lwz     r9, S_REGS(r31)
        slwi    r3, r3, 2
        addi    r3, r3, R_RES
        lwzx    r3, r9, r3
        blr

# r3 = the depth mode of r3 bits per pixel (1 bit for anything else).
mode_of:
        li      r4, FIRST_MODE
        li      r5, 1
1:      cmpw    r5, r3
        beq     2f
        slwi    r5, r5, 1
        addi    r4, r4, 1
        cmpwi   r5, 32
        ble     1b
        li      r4, FIRST_MODE
2:      mr      r3, r4
        blr

# r3 = the row bytes of width r3 at depth mode r4: whole longwords.
stride_of:
        subi    r4, r4, FIRST_MODE
        slw     r3, r3, r4
        addi    r3, r3, 31
        srwi    r3, r3, 5
        slwi    r3, r3, 2
        blr

# r3 = 0 when depth mode r3 is valid and resolution r4 at that depth fits
# the VRAM, else -1.
depth_fits:
        ENTER   64
        cmpwi   r3, FIRST_MODE
        blt     1f
        cmpwi   r3, FIRST_MODE + 5
        bgt     1f
        mr      r10, r3
        mr      r3, r4
        bl      res_entry
        andi.   r11, r3, 0xFFFF         # height
        srwi    r3, r3, 16
        mr      r4, r10
        bl      stride_of
        mullw   r3, r3, r11
        lwz     r0, S_VRAM(r31)
        cmplw   r3, r0
        bgt     1f
        li      r3, 0
        LEAVE   64
1:      li      r3, -1
        LEAVE   64

# Resolution r3: S_RES, S_WIDTH, S_HEIGHT and the card's WIDTH, HEIGHT.
set_res:
        ENTER   64
        sth     r3, S_RES(r31)
        bl      res_entry
        lwz     r9, S_REGS(r31)
        srwi    r0, r3, 16
        sth     r0, S_WIDTH(r31)
        stw     r0, R_WIDTH(r9)
        andi.   r0, r3, 0xFFFF
        sth     r0, S_HEIGHT(r31)
        stw     r0, R_HEIGHT(r9)
        eieio
        LEAVE   64

# Depth mode r3 at the current resolution: the card's registers, the CLUT
# at 50% gray. r3 = 0, or -1 when it does not fit.
set_depth:
        ENTER   64
        stw     r3, 56(r1)
        lhz     r4, S_RES(r31)
        bl      depth_fits
        cmpwi   r3, 0
        bne     9f
        lwz     r4, 56(r1)
        sth     r4, S_MODE(r31)
        subi    r5, r4, FIRST_MODE
        li      r6, 1
        slw     r6, r6, r5
        sth     r6, S_DEPTH(r31)
        lbz     r0, S_FLAGS(r31)
        andi.   r0, r0, 0xFF - F_DIRECT
        cmpwi   r6, 16
        blt     1f
        ori     r0, r0, F_DIRECT
1:      stb     r0, S_FLAGS(r31)
        lhz     r3, S_WIDTH(r31)
        bl      stride_of
        sth     r3, S_STRIDE(r31)
        lwz     r9, S_REGS(r31)
        stw     r3, R_STRIDE(r9)
        li      r0, FB_BASE
        stw     r0, R_BASE(r9)
        lhz     r0, S_DEPTH(r31)
        stw     r0, R_DEPTH(r9)
        li      r0, 0
        stw     r0, R_CLUT_INDEX(r9)
        eieio
        li      r0, 0x7F                # every CLUT entry 50% gray
        li      r5, 3 * 256
2:      stw     r0, R_CLUT_DATA(r9)
        subic.  r5, r5, 1
        bne     2b
        eieio
        li      r3, 0
9:      LEAVE   64

# SetEntries and DirectSetEntries: csTable, csStart (-1: by the value
# fields), csCount (zero based); indexed depths reach their 2^depth
# entries. r3 = the result.
set_entries:
        lwz     r4, 0(r29)              # csTable
        lha     r5, 4(r29)              # csStart
        lha     r6, 6(r29)              # csCount
        cmpwi   r6, 0
        blt     9f
        li      r7, 256                 # entries reachable
        lbz     r12, S_FLAGS(r31)
        andi.   r0, r12, F_DIRECT
        bne     1f
        lhz     r0, S_DEPTH(r31)
        li      r7, 1
        slw     r7, r7, r0
1:      cmpwi   r5, 0
        blt     3f
        add     r0, r5, r6
        cmpw    r0, r7
        bge     9f
3:      cmpwi   r5, 0
        blt     4f
        mr      r0, r5
        addi    r5, r5, 1
        b       5f
4:      lhz     r0, 0(r4)               # value
        cmpw    r0, r7
        bge     9f
5:      andi.   r0, r0, 0xFF
        stw     r0, R_CLUT_INDEX(r28)
        eieio
        lbz     r8, 2(r4)               # red, green, blue: the high bytes
        lbz     r9, 4(r4)
        lbz     r10, 6(r4)
        andi.   r0, r12, F_GRAY
        beq     6f
        andi.   r0, r12, F_DIRECT
        bne     6f
        mulli   r8, r8, 77              # luminance: 0.30 R + 0.59 G + 0.11 B
        mulli   r9, r9, 151
        mulli   r10, r10, 28
        add     r8, r8, r9
        add     r8, r8, r10
        srwi    r8, r8, 8
        mr      r9, r8
        mr      r10, r8
6:      stw     r8, R_CLUT_DATA(r28)
        stw     r9, R_CLUT_DATA(r28)
        stw     r10, R_CLUT_DATA(r28)
        eieio
        addi    r4, r4, 8
        subic.  r6, r6, 1
        bge     3b
        li      r3, 0
        blr
9:      li      r3, controlErr
        blr

# The storage's gamma table header: three channels of 256 bytes, no formula.
gamma_header:
        li      r0, 0
        stw     r0, S_GAMMA(r31)        # gVersion, gType
        sth     r0, S_GAMMA+4(r31)      # gFormulaSize
        li      r0, 3
        sth     r0, S_GAMMA+6(r31)      # gChanCnt
        li      r0, 256
        sth     r0, S_GAMMA+8(r31)      # gDataCnt
        li      r0, 8
        sth     r0, S_GAMMA+10(r31)     # gDataWidth
        blr

gamma_linear:
        ENTER   64
        bl      gamma_header
        addi    r4, r31, S_GAMMA + 12
        li      r6, 0
1:      stbx    r6, r4, r6
        addi    r7, r6, 256
        stbx    r6, r4, r7
        addi    r7, r6, 512
        stbx    r6, r4, r7
        addi    r6, r6, 1
        cmpwi   r6, 256
        blt     1b
        LEAVE   64

# The storage's table into the card.
gamma_load:
        lwz     r9, S_REGS(r31)
        li      r0, 0
        stw     r0, R_GAMMA_INDEX(r9)
        eieio
        addi    r4, r31, S_GAMMA + 12
        li      r6, 0
1:      lbzx    r0, r4, r6
        stw     r0, R_GAMMA_DATA(r9)
        addi    r7, r6, 256
        lbzx    r0, r4, r7
        stw     r0, R_GAMMA_DATA(r9)
        addi    r7, r6, 512
        lbzx    r0, r4, r7
        stw     r0, R_GAMMA_DATA(r9)
        addi    r6, r6, 1
        cmpwi   r6, 256
        blt     1b
        eieio
        blr

# The screen filled with the depth's 50% dither: rows alternate between the
# depth's two patterns, each a pair of longwords.
gray_fill:
        lwz     r3, S_BASE(r31)
        lhz     r4, S_MODE(r31)
        subi    r4, r4, FIRST_MODE
        slwi    r4, r4, 4
        LA      r5, dither
        add     r5, r5, r4
        lhz     r8, S_HEIGHT(r31)
        lhz     r7, S_STRIDE(r31)
        srwi    r7, r7, 2               # longwords per row
        li      r9, 0                   # 0 on even rows, 8 on odd ones
1:      lwzx    r10, r5, r9
        addi    r0, r9, 4
        lwzx    r11, r5, r0
        mtctr   r7
2:      stw     r10, 0(r3)
        addi    r3, r3, 4
        mr      r0, r10
        mr      r10, r11
        mr      r11, r0
        bdnz    2b
        xori    r9, r9, 8
        subic.  r8, r8, 1
        bne     1b
        blr

# A linear ramp in each channel of the CLUT (direct depths).
clut_ramp:
        lwz     r9, S_REGS(r31)
        li      r0, 0
        stw     r0, R_CLUT_INDEX(r9)
        eieio
        li      r5, 0
1:      stw     r5, R_CLUT_DATA(r9)
        stw     r5, R_CLUT_DATA(r9)
        stw     r5, R_CLUT_DATA(r9)
        addi    r5, r5, 1
        cmpwi   r5, 256
        blt     1b
        eieio
        blr

        .balign 16
code_end:

# --- data -----------------------------------------------------------------------

data_base:
import_table:
        .space  4 * IMPORTS
dodriverio_tvec:
        .long   DoDriverIO - code_base  # relocated by the code section's address
        .long   0                       # and the data section's (the TOC)
vbl_isr_tvec:
        .long   vbl_isr - code_base
        .long   0

# TheDriverDescription (Designing PCI Cards, chapter 8, "Driver description
# structure"): its signature and version; the driver type (the device's
# "name", the driver's version); the run-time options
# (kDriverIsOpenedUponLoad | kDriverIsUnderExpertControl, as a display's)
# and the unit table name; one service, 'ndrv' 'vido'.
the_driver_description:
        .long   0x6D74656A              # 'mtej'
        .long   0                       # kInitialDriverDescriptor
        .byte   driver_name_end - driver_name   # Str31: the length, the name, zeros to 32 bytes
driver_name:
        .ascii  "Livadix,Display"
driver_name_end:
        .space  31 - (driver_name_end - driver_name), 0
        .long   0x01008000              # 1.0 final
        .long   6
        .byte   unit_name_end - unit_name       # Str31
unit_name:
        .ascii  ".Display_Video_Livadix"
unit_name_end:
        .space  31 - (unit_name_end - unit_name), 0
        .space  32, 0                   # driverDescReserved
        .long   1                       # nServices
        .long   0x6E647276              # 'ndrv'
        .long   0x7669646F              # 'vido'
        .long   0x01008000

str_aapl_address:   .asciz  "AAPL,address"
str_assigned:       .asciz  "assigned-addresses"
str_driver_ist:     .asciz  "driver-ist"
str_preferred:      .asciz  "livadix,preferred"
str_accel:          .byte   29
                    .ascii  "Livadix Accelerated QuickDraw"

# The accelerated QuickDraw's code fragment (livadixcard_accel_pef.yaml).
        .balign 16
accel_pef:
        .incbin "../bin/livadixcard_accel_pef.bin"
        .balign 4
accel_pef_end:

        .balign 4
# Per depth: the even row's two longwords, the odd row's.
dither:
        .long   0xAAAAAAAA, 0xAAAAAAAA, 0x55555555, 0x55555555     # 1 bit: black, white
        .long   0xCCCCCCCC, 0xCCCCCCCC, 0x33333333, 0x33333333     # 2
        .long   0xF0F0F0F0, 0xF0F0F0F0, 0x0F0F0F0F, 0x0F0F0F0F     # 4
        .long   0xFF00FF00, 0xFF00FF00, 0x00FF00FF, 0x00FF00FF     # 8
        .long   0x00007FFF, 0x00007FFF, 0x7FFF0000, 0x7FFF0000     # 16: black, white
        .long   0x00000000, 0x00FFFFFF, 0x00FFFFFF, 0x00000000     # 32

        .balign 4
storage:
        .space  S_SIZE, 0

data_end:
