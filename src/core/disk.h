#ifndef MC_DISK_H
#define MC_DISK_H
#include <stddef.h>
#include <stdint.h>
typedef struct mc_disk mc_disk;
/* Host backing store. A writable disk writes guest writes into the image file
 * itself; otherwise the image is opened read-only. ISO media use 2048-byte
 * sectors and are always read-only: an image of the data sectors, an image
 * of raw 2352-byte sectors (Mode 1 or Mode 2 form 1; the data is taken out of
 * each), or a cue sheet naming either (its first track). A DiskCopy 4.2 image (400, 720, 800 or 1440
 * KiB) is recognised by its header; its data checksum is kept current. */
mc_disk *mc_disk_open(const char *image, int writable, uint32_t sector_size,
                      char *error, size_t capacity);
void mc_disk_close(mc_disk *disk);
/* Whether PATH names the file this disk has open. */
int mc_disk_same_file(const mc_disk *disk, const char *path);
/* For a .bin CD image with a cue sheet of the same name beside it, the
 * cue sheet's path in buffer (which keeps its audio tracks); else path. */
const char *mc_disk_cue_sheet(const char *path, char *buffer, size_t capacity);
uint64_t mc_disk_sectors(const mc_disk *disk);
/* 512, or 2048 for ISO media. */
uint32_t mc_disk_sector_size(const mc_disk *disk);
int mc_disk_readonly(const mc_disk *disk);
/* Whether the image is a floppy: 400, 720, 800 or 1440 KiB of 512-byte sectors
 * (raw or DiskCopy 4.2). */
int mc_disk_floppy(const mc_disk *disk);
const char *mc_disk_label(const mc_disk *disk);
int mc_disk_read(mc_disk *disk, uint64_t sector, uint32_t count, void *buffer);
int mc_disk_write(mc_disk *disk, uint64_t sector, uint32_t count, const void *buffer);
/* Byte ranges; a write leaves the rest of its sectors untouched. */
int mc_disk_read_bytes(mc_disk *disk, uint64_t offset, uint32_t count, void *buffer);
int mc_disk_write_bytes(mc_disk *disk, uint64_t offset, uint32_t count, const void *buffer);
/* DiskCopy 4.2 tags: 12 bytes per sector. Whether the image carries them;
 * reads give zeros and writes are accepted and dropped when it does not. A
 * write keeps the header's tag checksum current. */
int mc_disk_has_tags(const mc_disk *disk);
int mc_disk_read_tags(mc_disk *disk, uint64_t sector, uint32_t count, void *buffer);
int mc_disk_write_tags(mc_disk *disk, uint64_t sector, uint32_t count, const void *buffer);
/* Guest writes go into the image file at once (handed to the host system)
 * and are made durable (flushed to the storage device) later: by
 * mc_disk_sync when some write has waited about a second, or `all` (the
 * guest is idle), by mc_disk_close (eject, power off) and at exit. */
void mc_disk_sync(int all);

/* The guest's PREVENT MEDIUM REMOVAL on the drive holding the disk: a real
 * drive ignores its eject button then (SCSI-2 9.2.4). */
void mc_disk_set_locked(mc_disk *disk, int locked);
int mc_disk_locked(const mc_disk *disk);

/* CD tracks (a cue sheet's; an image of data alone is one data track).
 * Addresses and lengths are in frames of 1/75 s (a 2048-byte block), disc
 * logical block 0 at track 1's start. */
typedef struct mc_disk_track { unsigned number; int audio; uint32_t start, length; } mc_disk_track;
unsigned mc_disk_tracks(const mc_disk *disk);
int mc_disk_track_get(const mc_disk *disk, unsigned index, mc_disk_track *track);
/* The lead-out: the frame after the last track. */
uint32_t mc_disk_leadout(const mc_disk *disk);
/* The track a frame belongs to (its pregap counts as its own). */
int mc_disk_track_at(const mc_disk *disk, uint32_t lba, mc_disk_track *track);
/* One frame of audio: 588 stereo 16-bit samples (zeros outside the audio
 * tracks). 0 past the lead-out or on a disc without a cue sheet. */
int mc_disk_read_audio(mc_disk *disk, uint32_t lba, int16_t *stereo);

/* The CD player: the drive's commands start, pause and stop it; the
 * machine's sound output mixes it in. Its clock is the guest's time in
 * samples of 1/44100 s, which the machine sets when it takes the disc. */
enum { MC_CD_IDLE, MC_CD_PLAYING, MC_CD_PAUSED, MC_CD_COMPLETED };
void mc_disk_set_clock(mc_disk *disk, uint64_t (*clock)(void *ctx), void *ctx);
/* Plays frames [start, end); 0 when start is not in an audio track. */
int mc_disk_play(mc_disk *disk, uint32_t start, uint32_t end);
/* Scan a contiguous audio region at 8x, playing normal-pitch snippets. */
int mc_disk_scan(mc_disk *disk, uint32_t start, int reverse);
/* Returns 1 when an active scan becomes normal playback in place. */
int mc_disk_stop_scan(mc_disk *disk);
void mc_disk_pause(mc_disk *disk, int pause);
void mc_disk_stop(mc_disk *disk);
/* Puts the head at `lba`, the current location a play from there (a
 * starting address of FFh...) starts at. Play stops, or with `hold` a play
 * in progress or paused waits there, paused, when `lba` is audio. */
void mc_disk_seek(mc_disk *disk, uint32_t lba, int hold);
/* MC_CD_*, and the frame playing (or where it paused or ended). */
int mc_disk_play_state(mc_disk *disk, uint32_t *lba);
/* Output volume per channel, 0-255 (the drive's audio control page). */
void mc_disk_set_volume(mc_disk *disk, uint8_t left, uint8_t right);
/* Mixes the playing audio into `stereo` at `rate`: `have` frames are the
 * machine's sound; up to `frames` in all are returned, the rest of them
 * silence and the disc's audio, as far as guest time has gone. */
size_t mc_disk_mix(mc_disk *disk, int16_t *stereo, size_t have, size_t frames, uint32_t rate);
#endif
