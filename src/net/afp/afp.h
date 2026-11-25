/* Livadix — an AFP 2.1 file server (AppleTalk Filing Protocol, Inside
 * AppleTalk 2nd ed. chapter 13, and "AppleTalk Filing Protocol Version 2.1
 * and 2.2") serving folders of the host as volumes, for ASP (asp.h).
 *
 * Volumes are Windows folders read per request: what Windows changes shows
 * at once, and a watch on each folder moves the modification dates of the
 * folders that changed (and of the volume), so the Finder sees it within
 * its next check. Concurrent writes from both sides: the last writer wins.
 *
 * Livadix's own code. */
#ifndef MC_AFP_H
#define MC_AFP_H

#include "afp_meta.h"

#define MC_AFP_VOLUMES 8
#define MC_AFP_PATH 1024

typedef struct mc_afp_volume_config {
    char name[64];             /* UTF-8; at most 27 bytes in Mac Roman */
    char path[MC_AFP_PATH];    /* UTF-8 */
    int readonly;
} mc_afp_volume_config;

typedef struct mc_afp_config {
    char server_name[64];      /* UTF-8; at most 31 bytes in Mac Roman */
    int metadata;              /* MC_META_STREAMS or MC_META_APPLEDOUBLE */
    char password[16];         /* empty: guests; else any name with it (at most 8 bytes) */
    size_t volume_count;
    mc_afp_volume_config volumes[MC_AFP_VOLUMES];
    /* where the databases of read-only volumes go (UTF-8; NULL or empty:
       %LOCALAPPDATA%\Livadix\afp) */
    const char *state_folder;
    void (*log)(void *ctx, const char *line);
    void *log_ctx;
} mc_afp_config;

typedef struct mc_afp mc_afp;

/* Opens the volumes. NULL with a message when one cannot be served. */
mc_afp *mc_afp_create(const mc_afp_config *config, char *error, size_t capacity);
void mc_afp_destroy(mc_afp *afp);
/* The server's name in Mac Roman (a Pascal string's bytes) for NBP. */
const char *mc_afp_server_name(const mc_afp *afp, size_t *length);
/* What a volume uses: its metadata mode (after the NTFS check) and a note
 * ("" or why AppleDouble was chosen). */
int mc_afp_volume_metadata(const mc_afp *afp, unsigned volume, const char **note);

/* For ASP (asp.h): FPGetSrvrInfo's block, sessions, commands and writes. */
size_t mc_afp_status(mc_afp *afp, uint8_t *out, size_t capacity);
void *mc_afp_session_open(mc_afp *afp);
void mc_afp_session_close(mc_afp *afp, void *session);
int32_t mc_afp_command(mc_afp *afp, void *session, const uint8_t *cmd, size_t length, uint8_t *reply, size_t *reply_length);
long mc_afp_write_begin(mc_afp *afp, void *session, const uint8_t *cmd, size_t length, int32_t *result, uint8_t *reply,
                        size_t *reply_length);
int32_t mc_afp_write_end(mc_afp *afp, void *session, const uint8_t *cmd, size_t length, const uint8_t *data, size_t data_length,
                         uint8_t *reply, size_t *reply_length);

/* Serves the folder watches: changes made on the host move modification
 * dates. Never blocks. */
void mc_afp_poll(mc_afp *afp);
/* A change at a path relative to a volume's folder (the watch calls it;
 * tests may too). */
void mc_afp_host_changed(mc_afp *afp, unsigned volume, const uint16_t *relative);

/* AFP dates: seconds since 2000-01-01 00:00 UTC. */
int32_t mc_afp_time(int64_t unix_seconds);

#endif
