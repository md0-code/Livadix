/* Livadix — a user-mode NAT between the Macintosh's Ethernet and the host's
 * sockets: the Macintosh sees a small
 * Ethernet with a router; what it sends beyond the router leaves through
 * ordinary sockets of the host, so nothing is installed and no rights are
 * needed.
 *
 * The virtual network (Livadix's choice, the addresses most user-mode NATs
 * use): 10.0.2.0/24; the router and the host at 10.0.2.2 (connections to it
 * reach the host's own 127.0.0.1), the DNS server at 10.0.2.3, the
 * Macintosh at 10.0.2.15, given out by DHCP and BOOTP. Both virtual hosts
 * answer at one Ethernet address, 02:4C:56:44:58:02 (locally administered).
 *
 * Sources: RFC 826 (ARP), RFC 791 (IP), RFC 792 (ICMP
 * echo), RFC 768 (UDP), RFC 793 and RFC 9293 (TCP: the state machine,
 * sequence space, window, MSS option, reset generation), RFC 6298 (the
 * retransmission timer), RFC 5681 3.2 (fast retransmit), RFC 951 and RFC
 * 1497 (BOOTP and its vendor extensions), RFC 2131 and RFC 2132 (DHCP and
 * its options), RFC 1035 (DNS messages and TCP framing), RFC 7766 (DNS TCP connection
 * reuse), RFC 1122 3.3.2 (IP reassembly), RFC 1071 (the checksum).
 *
 * What it does:
 *   - ARP: answers for 10.0.2.2 and 10.0.2.3.
 *   - DHCP (DISCOVER/OFFER, REQUEST/ACK or NAK, INFORM) and BOOTP: the
 *     Macintosh's address, mask, router, DNS server, a day's lease.
 *   - IPv4: fragments reassembled; output fragmented at a 1500-byte MTU.
 *   - DNS over UDP and TCP at 10.0.2.3: A queries answered from the host's resolver
 *     (getaddrinfo on a worker thread; 127.x answers become 10.0.2.2), AAAA
 *     with no record, other types "not implemented".
 *   - ICMP echo: 10.0.2.2 and 10.0.2.3 answer at once; elsewhere through
 *     the host's ICMP echo service (IcmpSendEcho on Windows) on the worker.
 *   - UDP: one host socket per Macintosh port; replies come back from the
 *     address they came from. Idle for two minutes, it closes.
 *   - TCP: the Macintosh's connection ends here, at a full TCP of its own
 *     (three-way handshake once the host's connect succeeded, else RST;
 *     sliding window both ways, MSS, out-of-order segments held and merged,
 *     retransmission on a timer with exponential backoff and on three
 *     duplicate ACKs, zero-window probes, FIN both ways and RST), and the
 *     bytes go through a host socket.
 *   - Port forwards: a host TCP or UDP port leads to a Macintosh port;
 *     connections arrive from 10.0.2.2.
 *
 * The NAT is single-threaded: mc_nat_input takes the Macintosh's frames,
 * mc_nat_poll serves the sockets and timers, frames for the Macintosh go
 * to the output callback; both from one thread (net.h runs it on a thread
 * of its own). Timers count wall time (the clock function).
 *
 * Livadix's own code. */
#ifndef MC_NAT_H
#define MC_NAT_H

#include <stddef.h>
#include <stdint.h>

#define MC_NAT_FORWARDS 16

typedef struct mc_nat_forward {
    int udp;                    /* 0: TCP, 1: UDP */
    uint32_t bind;              /* the host address it listens on (127.0.0.1 by default) */
    uint16_t host_port, guest_port;
} mc_nat_forward;

typedef struct mc_nat_config {
    /* Addresses (host byte order); zero for the defaults above. */
    uint32_t network, router, dns, guest;
    /* What connections to the router reach on the host (default 127.0.0.1). */
    uint32_t host_loopback;
    mc_nat_forward forward[MC_NAT_FORWARDS];
    unsigned forwards;
    /* Wall time in milliseconds (NULL: the host's monotonic clock). */
    uint64_t (*clock)(void *ctx);
    void *clock_ctx;
    /* Frames for the Macintosh. */
    void (*output)(void *ctx, const uint8_t *frame, size_t length);
    void *output_ctx;
    /* Optional: one line per event (DHCP lease, connection opened, closed). */
    void (*log)(void *ctx, const char *line);
    void *log_ctx;
} mc_nat_config;

typedef struct mc_nat_stats {
    unsigned dhcp_offers, dhcp_acks, bootp_replies, arp_replies, dns_queries, dns_answers;
    unsigned udp_out, udp_in, tcp_opened, tcp_refused, tcp_closed, tcp_retransmits, icmp_replies;
    uint64_t tcp_bytes_out, tcp_bytes_in;     /* Macintosh to host, host to Macintosh */
} mc_nat_stats;

typedef struct mc_nat mc_nat;

/* NULL with a message when a socket cannot be made (or a forward's port
 * cannot be bound). */
mc_nat *mc_nat_create(const mc_nat_config *config, char *error, size_t capacity);
void mc_nat_destroy(mc_nat *nat);
/* A frame from the Macintosh (destination address to the end of the data). */
void mc_nat_input(mc_nat *nat, const uint8_t *frame, size_t length);
/* Serves the sockets and timers, waiting up to timeout_ms for something to
 * happen (or for mc_nat_wake). */
void mc_nat_poll(mc_nat *nat, int timeout_ms);
/* Ends a wait in mc_nat_poll; any thread may call it. */
void mc_nat_wake(mc_nat *nat);
void mc_nat_get_stats(mc_nat *nat, mc_nat_stats *stats);
/* The port a forward listens on (its host_port, or the one the host chose
 * for host_port 0), or 0. */
uint16_t mc_nat_forward_port(mc_nat *nat, unsigned index);

#endif
