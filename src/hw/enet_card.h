/* Livadix — Apple's Ethernet cards for the models without built-in Ethernet
 * (docs/NOTES.md "Network").
 *
 * Two real cards, each with its own declaration ROM from the roms folders
 * (docs/NOTES.md) and the driver the System's network software carries:
 *
 * - The EtherTalk NB Card (Apple/3Com 670-0210, BoardId 8; System 7.5's
 *   'enet' 8, "EtherTalk NB Driver"), for NuBus slots: a National DP8390
 *   (dp8390.h) with 16 KB of buffer memory. Observed from its declaration
 *   ROM and driver: the ROM on byte lanes 0 and 2
 *   (ByteLanes A5h: the even addresses), the station address in its
 *   first six bytes (the vendor sResource field 128 of the Ethernet
 *   sResource points there);
 *   MinorBaseOS D0000h, the buffer memory there (pages 00h-3Fh: PSTART 3,
 *   PSTOP 40h), the NIC's registers at +10000h (E0000h), register n a byte
 *   at (15 - n) * 4 (the driver writes CR at +3Ch, IMR at +0, DCR at +4);
 *   the NIC's INT on the slot's /NMRQ, acknowledged in the ISR alone. The
 *   card decodes A19-A0 only, as a card of the 24-bit Macintosh II family
 *   must (Designing Cards and Drivers, 3rd ed., chapter 8).
 * - The Ethernet LC Card (341-0842, BoardId 2Eh; its own .ENET in the
 *   declaration ROM, System 7.5's 'enet' 46), for the LC processor-direct
 *   slot (slot $E): a National DP83932 SONIC (sonic.h), bus master in the
 *   Macintosh's RAM. Observed from the ROM's driver: the SONIC's registers
 *   at slot offset 0, register n at n * 4 (RCR at +8: it steps RCR's
 *   loopback bits through MAC, ENDEC and transceiver loopback at open);
 *   the station address PROM at +400000h, six bytes at consecutive
 *   addresses, each bit-reversed; the ROM on all four byte lanes.
 *
 * Livadix's own code. */
#ifndef MC_ENET_CARD_H
#define MC_ENET_CARD_H

#include "hw_machine.h"
#include "nubus.h"

#include <stddef.h>
#include <stdint.h>

/* Which card a slot takes (network.card's nubus and lc). */
enum { MC_ENET_CARD_NUBUS = 1, MC_ENET_CARD_LC = 2 };

typedef struct mc_enet_card mc_enet_card;

/* The card network.card asks for, in `slot` (0: the slot is taken) of a
 * board whose slots take `kind`. NULL when no card is fitted: network.card
 * none, or auto without the card's declaration ROM or a free slot. An
 * explicit card that cannot be fitted leaves a message in hw->error. */
mc_enet_card *mc_enet_card_fit(mc_hw *hw, mc_nubus *nubus, unsigned kind, unsigned slot);
/* The same for a board that is not an mc_hw (the PowerPC boards): its
 * configuration, its name for messages, its host and bus, and where a
 * message goes. */
typedef struct mc_enet_card_env {
    const mc_config *config;
    const char *name;
    const mc_hw_host *host;
    mc_bus *bus;
    char *error;
    size_t error_size;
} mc_enet_card_env;
mc_enet_card *mc_enet_card_fit_on(const mc_enet_card_env *env, mc_nubus *nubus, unsigned kind, unsigned slot);
/* All of these accept NULL (no card). */
void mc_enet_card_reset(mc_enet_card *c);
void mc_enet_card_net_host(mc_enet_card *c, const mc_net_host *host);
int mc_enet_card_net_receive(mc_enet_card *c, const uint8_t *frame, size_t length);
void mc_enet_card_free(mc_enet_card *c);

#endif
