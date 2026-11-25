| livadixdisk_ata — the ATA flavour of Livadix's block driver:
| livadixdisk.s assembled with ATA defined. Built by tools/guest_build.py into
| src/hw/guest/bin/livadixdisk_ata.bin and placed in the virtual disk's
| Apple_Driver_ATA partition (src/hw/storage/vdisk.c).
        .set    ATA, 1
        .include "livadixdisk.s"
