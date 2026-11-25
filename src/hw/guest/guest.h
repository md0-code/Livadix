/* Livadix — Livadix's own guest code built into the executable. The images are assembled from src/hw/guest by
 * tools/guest_build.py, committed in src/hw/guest/bin and turned into C
 * arrays at configure time. */
#ifndef MC_GUEST_H
#define MC_GUEST_H

#include <stddef.h>
#include <stdint.h>

/* livadixdisk: the SCSI (disk and CD-ROM) block driver for the virtual disk's
 * driver partition (storage/vdisk.h, mc_vdisk_drivers.scsi). */
const uint8_t *mc_guest_livadixdisk(size_t *size);
/* livadixdisk_ata: the ATA flavour of the same driver (ATA hard disks and ATAPI CD-ROM drives
 * through _ATAManager), for the Apple_Driver_ATA partition (mc_vdisk_drivers.ata). */
const uint8_t *mc_guest_livadixdisk_ata(size_t *size);
/* livadixcard: the Livadix Display Card's declaration ROM (livadix_card.h), its
 * driver and PrimaryInit inside. */
const uint8_t *mc_guest_livadixcard(size_t *size);
/* livadixcard24: the same card for 24-bit QuickDraw (its video sResources not
 * f32BitMode, src/hw/guest/declrom/livadixcard24.yaml): the Macintosh II
 * family's. */
const uint8_t *mc_guest_livadixcard24(size_t *size);
/* livadixcard_pci: the PCI Livadix Display Card's Open Firmware FCode expansion
 * ROM and PowerPC ndrv (src/hw/guest/pci/livadixcard_pci.yaml). */
const uint8_t *mc_guest_livadixcard_pci(size_t *size);

#endif
