/* Livadix — the Ethernet segment beside the emulated Macintosh: a hub that the
 * Macintosh's Ethernet controller and any number of virtual hosts attach
 * to. Every frame on the segment reaches every other station (hosts filter
 * by destination, as Ethernet stations do), so broadcasts and multicasts
 * (AppleTalk's 09:00:07:FF:FF:FF and its zone multicasts among them) reach
 * every host. The user-mode NAT (nat.h) is one such host; others (an
 * AppleTalk server, for instance) attach the same way.
 *
 * Threads: the hosts run on the segment's own thread, so a slow host
 * socket never holds up the emulation. The emulator's thread hands the
 * frames the Macintosh transmits to mc_net_send and offers the frames
 * waiting for it to the machine with mc_net_pump (or mc_net_next and
 * mc_net_pop); both queues are guarded by a lock.
 *
 * A host (mc_net_host_ops), all called on the segment's thread:
 *   input(host, frame, length)  a frame another station put on the segment
 *                               (destination address to the end of the
 *                               data, no FCS);
 *   poll(host, timeout_ms)      serve the host's own work (sockets,
 *                               timers), at least every 20 ms; the first
 *                               host with a wake may wait up to timeout_ms
 *                               in it for its work or for wake; the others
 *                               are polled with 0 and must not block;
 *   wake(host)                  any thread: end that wait (optional; NULL
 *                               for a host that never waits);
 *   destroy(host)               after the thread has stopped.
 * A host puts its frames on the segment with mc_net_emit, from within its
 * input or poll.
 *
 * Livadix's own code. */
#ifndef MC_NET_H
#define MC_NET_H

#include "nat.h"
#include "../core/config.h"
#include "../core/machine.h"

#include <stddef.h>
#include <stdint.h>

#define MC_NET_HOSTS 8

typedef struct mc_net mc_net;

typedef struct mc_net_host_ops {
    void (*input)(void *host, const uint8_t *frame, size_t length);
    void (*poll)(void *host, int timeout_ms);
    void (*wake)(void *host);
    void (*destroy)(void *host);
} mc_net_host_ops;

/* An empty segment; hosts attach before mc_net_start. */
mc_net *mc_net_create(char *error, size_t capacity);
/* Attaches a host (owned by the segment from now on; destroyed with it).
 * 0 when the segment is full or started. */
int mc_net_attach_host(mc_net *net, const mc_net_host_ops *ops, void *host);
/* Starts the segment's thread. */
int mc_net_start(mc_net *net, char *error, size_t capacity);
/* Stops the thread, destroys the hosts, frees. */
void mc_net_close(mc_net *net);
/* A host's frame onto the segment: to the Macintosh and every other host.
 * Only on the segment's thread (from a host's input or poll). */
void mc_net_emit(mc_net *net, const void *from_host, const uint8_t *frame, size_t length);

/* The Macintosh's side (emulator thread). */
void mc_net_send(mc_net *net, const uint8_t *frame, size_t length);
/* The oldest frame for the Macintosh, copied into `frame` (at least 1514
 * bytes), left queued until mc_net_pop; 0 when none waits. */
int mc_net_next(mc_net *net, uint8_t *frame, size_t *length);
void mc_net_pop(mc_net *net);

/* The segment with the NAT on it, from a NAT configuration (its output is
 * the segment). NULL with a message on failure. */
mc_net *mc_net_open(const mc_nat_config *config, char *error, size_t capacity);
/* The same from a configuration (network.mode, network.forward): NULL
 * without a message when it is `none`. With LIVADIX_NET_LOG set in the
 * environment, the NAT's events go to stderr. */
mc_net *mc_net_open_config(const mc_config *config, char *error, size_t capacity);
/* The NAT's counters and a forward's listening port (opened by mc_net_open). */
void mc_net_stats(mc_net *net, mc_nat_stats *stats);
uint16_t mc_net_forward_port(mc_net *net, unsigned index);

/* Plugs the machine's Ethernet into the segment (net NULL: unplugged). */
void mc_net_attach(mc_net *net, mc_machine *machine);
/* Offers the machine the frames waiting for it, as long as it takes them. */
void mc_net_pump(mc_net *net, mc_machine *machine);

#endif
