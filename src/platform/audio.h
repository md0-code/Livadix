/* Host sound output: the machine's samples, pulled after each run slice and
 * resampled by SDL to the output device. */
#ifndef MC_HOST_AUDIO_H
#define MC_HOST_AUDIO_H

#include "../core/machine.h"

#include <SDL.h>
#include <stdio.h>

typedef struct mc_host_audio {
    SDL_AudioDeviceID device;
    SDL_AudioSpec output;
    SDL_AudioStream *stream;
    uint32_t rate;
    int started;
} mc_host_audio;

/* 0 when no output device opens; pumping then discards the samples. */
int mc_host_audio_open(mc_host_audio *audio);
/* Moves the machine's samples to the device; 0 on an SDL failure. */
int mc_host_audio_pump(mc_host_audio *audio, mc_machine *machine);
/* Bytes waiting in the device queue (tests). */
uint32_t mc_host_audio_queued(const mc_host_audio *audio);
void mc_host_audio_close(mc_host_audio *audio);

enum { MC_CAPTURE_FRAMES = 4096 };
typedef struct mc_host_capture {
    SDL_AudioDeviceID device;
    unsigned rate, read, count;
    int initialized;
    int16_t stereo[MC_CAPTURE_FRAMES * 2];
} mc_host_capture;
/* The default microphone opens only while the guest enables recording.
 * An unavailable device supplies silence; the callback buffer is bounded. */
mc_audio_source mc_host_capture_callbacks(mc_host_capture *capture);
void mc_host_capture_close(mc_host_capture *capture);

/* The machine's sound recorded to a WAV file instead (16-bit stereo PCM at
 * the machine's rate): mc_wav_pump after each run slice. */
typedef struct mc_wav {
    FILE *file;
    uint32_t rate, frames;
    int peak;                   /* loudest sample so far */
} mc_wav;
int mc_wav_open(mc_wav *wav, const char *path);
void mc_wav_pump(mc_wav *wav, mc_machine *machine);
/* Completes the header; 0 when the file could not be written. */
int mc_wav_close(mc_wav *wav);

#endif /* MC_HOST_AUDIO_H */
