/* Livadix's owned, transactional host NVRAM store. */
#ifndef MC_NVRAM_H
#define MC_NVRAM_H
#include "config.h"
typedef struct mc_nvram mc_nvram;
/* Startup media selects an owned state within the profile/model/ROM.
 * Boards receive a private legacy-format working file. Only a successful
 * machine session can replace the owned record. Built-in uses no file. */
int mc_nvram_begin(const mc_config *c, const char *base, uint32_t rom, unsigned payload_size,
                   mc_nvram **out, char *working, size_t capacity, char *error, size_t error_capacity);
int mc_nvram_end(mc_nvram *s, int commit, char *error, size_t capacity);
const char *mc_nvram_target(const mc_nvram *s);
/* Explicit migration by a caller that has verified the legacy file's
 * machine/ROM. Normal startup never imports an unowned file. */
int mc_nvram_import(const mc_config *c, const char *base, uint32_t rom, unsigned payload_size,
                    const char *legacy, char *error, size_t capacity);
int64_t mc_nvram_clock_offset(const uint8_t *little_endian);
#endif
