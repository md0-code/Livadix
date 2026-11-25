/* The stub machine (stub_machine.c): the machine interface (machine.h) for
 * models no real machine emulates yet. src/hw/machine.c forwards to it. */
#ifndef MC_STUB_MACHINE_H
#define MC_STUB_MACHINE_H

#include "machine.h"
#include "media.h"

typedef struct mc_stub mc_stub;

mc_stub *mc_stub_create(const mc_config *config, const uint8_t *rom, size_t rom_size, char *error, size_t capacity);
void mc_stub_destroy(mc_stub *m);
const char *mc_stub_description(const mc_stub *m);
const char *mc_stub_error(const mc_stub *m);
mc_run_status mc_stub_run(mc_stub *m, uint64_t cycles, uint64_t deadline);
uint64_t mc_stub_cycles(const mc_stub *m);
uint64_t mc_stub_time(const mc_stub *m);
uint64_t mc_stub_rate(const mc_stub *m);
int mc_stub_framebuffer(mc_stub *m, mc_framebuffer *fb);
int mc_stub_key(mc_stub *m, uint8_t adb_code, int down);
int mc_stub_mouse(mc_stub *m, int dx, int dy, int button);
uint32_t mc_stub_audio_rate(const mc_stub *m);
size_t mc_stub_audio_pull(mc_stub *m, int16_t *stereo, size_t frames);
unsigned mc_stub_drives(const mc_stub *m, mc_media_kind kind);
int mc_stub_insert(mc_stub *m, mc_media_kind kind, unsigned drive, mc_disk *disk, char *error, size_t capacity);
mc_disk *mc_stub_media(const mc_stub *m, mc_media_kind kind, unsigned drive, int *ejected);
int mc_stub_eject(mc_stub *m, mc_media_kind kind, unsigned drive);
int mc_stub_reinsert(mc_stub *m);

#endif
