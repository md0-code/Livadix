/* Livadix — the file server on the Ethernet segment: an AppleTalk node
 * (appletalk/atalk.h) with ASP (appletalk/asp.h) serving the AFP server
 * (afp/afp.h), registered with NBP as "<name>:AFPServer@*", so the
 * Macintosh's Chooser lists it under AppleShare.
 *
 * Livadix's own code. */
#ifndef MC_SHARE_H
#define MC_SHARE_H

#include "afp/afp.h"

#include <stddef.h>
#include <stdint.h>

typedef struct mc_net mc_net;
typedef struct mc_share mc_share;

/* Opens the shared folders; NULL with a message when one cannot be. */
mc_share *mc_share_create(const mc_afp_config *config, char *error, size_t capacity);
/* Puts the server on the segment (before mc_net_start); the segment owns it
 * from then on. 0 when the segment has no room. */
int mc_share_attach(mc_net *net, mc_share *share);
void mc_share_destroy(mc_share *share);
/* The server's AFP side (for --inspect and tests). */
mc_afp *mc_share_afp(mc_share *share);
/* The hardware address of the server's node. */
extern const uint8_t mc_share_mac[6];

#endif
