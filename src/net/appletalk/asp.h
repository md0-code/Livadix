/* Livadix — the server end of ASP, the AppleTalk Session Protocol (Inside
 * AppleTalk, 2nd ed., chapter 11), on ATP: the session listening socket
 * (GetStatus, OpenSess, tickles), one session server socket for every
 * session (Command, Write with WriteContinue, CloseSess, tickles), tickles
 * to the workstations every 30 seconds, sessions closed after 2 minutes of
 * silence, Attention and server-initiated CloseSess.
 *
 * Its client (the AFP server) answers through mc_asp_server_ops, on the
 * same thread.
 *
 * Livadix's own code. */
#ifndef MC_ASP_H
#define MC_ASP_H

#include "atp.h"

#define MC_ASP_QUANTUM (MC_ATP_PACKETS * MC_ATP_DATA_MAX)  /* 4624: QuantumSize */
#define MC_ASP_SESSIONS 16

/* SPError values (Inside AppleTalk, appendix C). */
enum { MC_ASP_BAD_VERSION = -1066, MC_ASP_SERVER_BUSY = -1071 };

typedef struct mc_asp mc_asp;

typedef struct mc_asp_server_ops {
    /* The service status block GetStatus returns (at most QuantumSize). */
    size_t (*status)(void *ctx, uint8_t *out, size_t capacity);
    /* A session opened: returns the client's context for it (NULL: refuse
     * with ServerBusy). */
    void *(*opened)(void *ctx, unsigned session_id, const mc_atalk_addr *workstation);
    /* The session ended (closed by either end, or timed out). */
    void (*closed)(void *ctx, void *session);
    /* A command: the reply (at most QuantumSize) and the 4-byte result. */
    int32_t (*command)(void *ctx, void *session, const uint8_t *cmd, size_t length, uint8_t *reply, size_t *reply_length);
    /* A write: returns how many bytes of data the command wants (the
     * WriteContinue's buffer size, at most QuantumSize), or -1 with the
     * result and reply to give at once. */
    long (*write_begin)(void *ctx, void *session, const uint8_t *cmd, size_t length, int32_t *result, uint8_t *reply,
                        size_t *reply_length);
    /* The write's data has arrived: the reply and result. */
    int32_t (*write_end)(void *ctx, void *session, const uint8_t *cmd, size_t length, const uint8_t *data, size_t data_length,
                         uint8_t *reply, size_t *reply_length);
} mc_asp_server_ops;

/* Opens the listening and session sockets on the node (dynamic sockets). */
mc_asp *mc_asp_create(mc_atalk *at, const mc_asp_server_ops *ops, void *ctx);
/* Closes every session (with CloseSess to the workstations) and the sockets. */
void mc_asp_destroy(mc_asp *asp, uint64_t now);
uint8_t mc_asp_listening_socket(const mc_asp *asp);
void mc_asp_tick(mc_asp *asp, uint64_t now);
/* Sends an attention code (not 0) to a session's workstation. 0 when the
 * session is gone. */
int mc_asp_attention(mc_asp *asp, void *session, uint16_t code, uint64_t now);
/* Closes a session from the server end. */
void mc_asp_close(mc_asp *asp, void *session, uint64_t now);
unsigned mc_asp_session_count(const mc_asp *asp);
/* Every request received, for LIVADIX_NET_LOG. */
void mc_asp_set_log(mc_asp *asp, void (*log)(void *ctx, const char *line), void *ctx);

#endif
