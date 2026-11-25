/* Livadix — the guest images (guest.h). */
#include "guest.h"

#include "mc_guest_images.h"

const uint8_t *mc_guest_livadixdisk(size_t *size) {
    if (size) *size = sizeof mc_guest_livadixdisk_bin;
    return mc_guest_livadixdisk_bin;
}

const uint8_t *mc_guest_livadixdisk_ata(size_t *size) {
    if (size) *size = sizeof mc_guest_livadixdisk_ata_bin;
    return mc_guest_livadixdisk_ata_bin;
}

const uint8_t *mc_guest_livadixcard(size_t *size) {
    if (size) *size = sizeof mc_guest_livadixcard_bin;
    return mc_guest_livadixcard_bin;
}

const uint8_t *mc_guest_livadixcard24(size_t *size) {
    if (size) *size = sizeof mc_guest_livadixcard24_bin;
    return mc_guest_livadixcard24_bin;
}

const uint8_t *mc_guest_livadixcard_pci(size_t *size) {
    if (size) *size = sizeof mc_guest_livadixcard_pci_bin;
    return mc_guest_livadixcard_pci_bin;
}
