/* Livadix — the AFP server's two databases of a shared volume, each a hidden
 * file in the shared folder (in %LOCALAPPDATA%\Livadix\afp for a read-only
 * share):
 *
 * .livadix-ids      the directory and file IDs: each CNode's ID with its
 *                parent's ID, its Windows name and the file system's file
 *                ID, so an ID stays with its file across runs and across
 *                renames and moves made on the Windows side (NTFS keeps a
 *                file's ID; elsewhere the parent and name are the key).
 *                Lines of UTF-8 appended as IDs change, compacted on open.
 * .livadix-desktop  the desktop database (Inside AppleTalk, chapter 13,
 *                "Desktop database"): icons by creator, type and icon
 *                type; applications by creator; comments by ID.
 *
 * Livadix's own code. */
#ifndef MC_AFP_STORE_H
#define MC_AFP_STORE_H

#include "afp_host.h"

#define MC_AFP_ROOT_PARENT 1
#define MC_AFP_ROOT 2
#define MC_AFP_FIRST_ID 17

typedef struct mc_afp_ids mc_afp_ids;
/* path NULL: kept in memory only. */
mc_afp_ids *mc_ids_open(const uint16_t *path, uint64_t root_index);
void mc_ids_close(mc_afp_ids *ids);
/* The ID of the CNode `name` in directory `parent` whose file ID is index
 * (0: unknown): the one it had (found by its file ID first, the record
 * then following a rename or move), else a new one. */
uint32_t mc_ids_get(mc_afp_ids *ids, uint32_t parent, const uint16_t *name, uint64_t index);
/* The ID of name in parent if it has one (0 otherwise). */
uint32_t mc_ids_find(mc_afp_ids *ids, uint32_t parent, const uint16_t *name);
/* What an ID stands for. 0 when unknown. */
int mc_ids_lookup(mc_afp_ids *ids, uint32_t id, uint32_t *parent, uint16_t name[MC_HOST_NAME], uint64_t *index);
/* The record of id now names `name` in `parent` (with its file ID, 0: as it was). */
void mc_ids_move(mc_afp_ids *ids, uint32_t id, uint32_t parent, const uint16_t *name, uint64_t index);
void mc_ids_forget(mc_afp_ids *ids, uint32_t id);
unsigned mc_ids_count(const mc_afp_ids *ids);

typedef struct mc_afp_desktop mc_afp_desktop;
mc_afp_desktop *mc_desktop_open(const uint16_t *path);
void mc_desktop_close(mc_afp_desktop *dt);
/* Icons. */
int mc_desktop_add_icon(mc_afp_desktop *dt, const uint8_t creator[4], const uint8_t type[4], uint8_t itype, uint32_t tag,
                        const uint8_t *bitmap, size_t length);
/* The icon's bitmap (NULL when none) and its length and tag. */
const uint8_t *mc_desktop_icon(mc_afp_desktop *dt, const uint8_t creator[4], const uint8_t type[4], uint8_t itype, size_t *length,
                               uint32_t *tag);
/* The index-th (from 1) icon of a creator. */
int mc_desktop_icon_info(mc_afp_desktop *dt, const uint8_t creator[4], unsigned index, uint8_t type[4], uint8_t *itype, uint32_t *tag,
                         size_t *length);
/* Applications: an application of `creator` named name (Mac Roman) in directory dir. */
int mc_desktop_add_appl(mc_afp_desktop *dt, const uint8_t creator[4], uint32_t dir, const uint8_t *name, size_t n, uint32_t tag);
int mc_desktop_remove_appl(mc_afp_desktop *dt, const uint8_t creator[4], uint32_t dir, const uint8_t *name, size_t n);
/* The index-th (from 1) application of a creator. */
int mc_desktop_appl(mc_afp_desktop *dt, const uint8_t creator[4], unsigned index, uint32_t *dir, uint8_t name[32], size_t *n,
                    uint32_t *tag);
/* Comments (at most 199 bytes) by ID. */
int mc_desktop_set_comment(mc_afp_desktop *dt, uint32_t id, const uint8_t *text, size_t n);
const uint8_t *mc_desktop_comment(mc_afp_desktop *dt, uint32_t id, size_t *n);
int mc_desktop_remove_comment(mc_afp_desktop *dt, uint32_t id);

#endif
