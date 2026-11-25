/* Livadix — an AppleTalk node on the Ethernet segment (EtherTalk Phase 2):
 * the link (802.3 + 802.2 LLC + SNAP), AARP (address probing and defence,
 * resolution, gleaning), DDP (extended headers, checksums verified when
 * present), NBP (the node's names answer LkUp; confirm is a directed LkUp)
 * and AEP (echo).
 *
 * The node is not a router. On an extended network without a router a node
 * keeps the provisional address it probed (in the startup range $FF00-$FFFE,
 * or the one it last had) and treats every network number as local (Inside AppleTalk, 2nd ed.,
 * chapter 4, "Node address acquisition on extended networks" and "DDP
 * routing algorithm"); the Macintosh does the same, so the two reach each
 * other directly and NBP lookups are broadcast on the cable. ZIP requests
 * are only answered by routers, so the node ignores them: the Chooser then
 * shows no zones and lists the servers of the cable.
 *
 * No clock of its own: every call that can time out takes the caller's
 * time in milliseconds. Not thread-safe; the segment's thread drives it.
 *
 * Livadix's own code. */
#ifndef MC_ATALK_H
#define MC_ATALK_H

#include <stddef.h>
#include <stdint.h>

/* DDP types (Inside AppleTalk, appendix C). */
enum { MC_DDP_RTMP = 1, MC_DDP_NBP = 2, MC_DDP_ATP = 3, MC_DDP_AEP = 4, MC_DDP_RTMP_REQ = 5, MC_DDP_ZIP = 6, MC_DDP_ADSP = 7 };
/* Statically assigned sockets. */
enum { MC_SOCKET_RTMP = 1, MC_SOCKET_NIS = 2, MC_SOCKET_ECHO = 4, MC_SOCKET_ZIS = 6 };
#define MC_DDP_DATA_MAX 586
#define MC_NBP_NAME_MAX 32

typedef struct mc_atalk mc_atalk;
typedef struct mc_atalk_addr {
    uint16_t net;
    uint8_t node, socket;
} mc_atalk_addr;

/* A datagram for an open socket: from its sender, of its DDP type. */
typedef void (*mc_atalk_receive_fn)(void *ctx, const mc_atalk_addr *from, uint8_t type, const uint8_t *data, size_t length,
                                    uint64_t now);
typedef void (*mc_atalk_output_fn)(void *ctx, const uint8_t *frame, size_t length);
typedef void (*mc_atalk_log_fn)(void *ctx, const char *line);

/* A node with the hardware address mac, putting its frames out through
 * output. seed varies the provisional address it tries first. */
mc_atalk *mc_atalk_create(const uint8_t mac[6], uint32_t seed, mc_atalk_output_fn output, void *output_ctx);
void mc_atalk_destroy(mc_atalk *at);
void mc_atalk_set_log(mc_atalk *at, mc_atalk_log_fn log, void *ctx);
/* With the log: every datagram in and out as well. */
void mc_atalk_set_trace(mc_atalk *at, int on);
/* Starts probing for an address (and again after a conflict). */
void mc_atalk_start(mc_atalk *at, uint64_t now);
/* A frame from the segment (destination address to the end of the data). */
void mc_atalk_input(mc_atalk *at, const uint8_t *frame, size_t length, uint64_t now);
/* Timers: probes, AARP requests and their queued datagrams. */
void mc_atalk_tick(mc_atalk *at, uint64_t now);
/* 1 once the node holds its address; the address (socket 0). */
int mc_atalk_ready(const mc_atalk *at);
mc_atalk_addr mc_atalk_address(const mc_atalk *at);

/* Opens a socket: a number from 1 to 254, or 0 for the next free dynamic
 * one (128-254). Returns the socket, 0 when taken or none is free. */
uint8_t mc_atalk_open(mc_atalk *at, uint8_t socket, mc_atalk_receive_fn receive, void *ctx);
void mc_atalk_close(mc_atalk *at, uint8_t socket);
/* Sends a datagram from one of the node's sockets (to node $FF: a
 * broadcast). 0 when it cannot be sent (no address yet, too long). */
int mc_atalk_send(mc_atalk *at, uint8_t from_socket, const mc_atalk_addr *to, uint8_t type, const uint8_t *data, size_t length,
                  uint64_t now);

/* NBP: registers object:type@* on a socket (names in Mac Roman, at most
 * 32 bytes each). 0 when the table is full or the name bad. */
int mc_nbp_register(mc_atalk *at, const char *object, const char *type, uint8_t socket);
void mc_nbp_remove(mc_atalk *at, uint8_t socket);
/* NBP's name match: case-insensitive in Mac Roman (Inside AppleTalk,
 * appendix D), '=' alone matches any name, one '≈' ($C5) any run of
 * characters. Pascal-free: pattern and name with their lengths. */
int mc_nbp_match(const uint8_t *pattern, size_t plen, const uint8_t *name, size_t nlen);

/* Counters for tests and the log. */
typedef struct mc_atalk_stats {
    unsigned probes_sent, aarp_replies, lookups_answered, datagrams_in, datagrams_out, bad_checksums, unresolved_drops;
} mc_atalk_stats;
void mc_atalk_get_stats(const mc_atalk *at, mc_atalk_stats *stats);

/* The DDP checksum over the bytes after the checksum field (Inside
 * AppleTalk, chapter 4, "DDP checksum computation"). */
uint16_t mc_ddp_checksum(const uint8_t *data, size_t length);

#endif
