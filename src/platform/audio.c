/* Host sound output (the platform layer, fitted to the machine
 * interface): SDL queues 48 kHz stereo; an SDL audio stream converts the
 * machine's rate to it. */
#include "audio.h"

#include <string.h>

int mc_host_audio_open(mc_host_audio *a) {
    memset(a, 0, sizeof *a);
    SDL_AudioSpec want = {0};
    want.freq = 48000; want.format = AUDIO_S16SYS; want.channels = 2; want.samples = 1024;
    a->device = SDL_OpenAudioDevice(NULL, 0, &want, &a->output, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
    if (!a->device) return 0;
    return 1;
}

static int drain(mc_host_audio *a) {
    int16_t samples[4096];
    int count;
    while ((count = SDL_AudioStreamAvailable(a->stream)) > 0) {
        if (count > (int)sizeof samples) count = (int)sizeof samples;
        count = SDL_AudioStreamGet(a->stream, samples, count);
        if (count < 0 || SDL_QueueAudio(a->device, samples, (Uint32)count)) return 0;
    }
    /* A short initial queue keeps normal scheduling jitter out of the DAC. */
    if (!a->started && SDL_GetQueuedAudioSize(a->device) >= (Uint32)a->output.freq * 4u / 25u) {
        SDL_PauseAudioDevice(a->device, 0);
        a->started = 1;
    }
    return count == 0;
}

int mc_host_audio_pump(mc_host_audio *a, mc_machine *m) {
    uint32_t rate = mc_machine_audio_rate(m);
    int16_t samples[2048 * 2];
    if (!rate) return 1;
    if (!a->device) {   /* no output: keep the machine's buffer empty */
        while (mc_machine_audio_pull(m, samples, 2048) == 2048) {}
        return 1;
    }
    if (a->rate != rate) {
        if (a->stream) {
            if (SDL_AudioStreamFlush(a->stream) || !drain(a)) return 0;
            SDL_FreeAudioStream(a->stream);
            a->stream = NULL;
        }
        a->stream = SDL_NewAudioStream(AUDIO_S16SYS, 2, (int)rate, a->output.format, a->output.channels, a->output.freq);
        if (!a->stream) return 0;
        a->rate = rate;
    }
    size_t frames;
    while ((frames = mc_machine_audio_pull(m, samples, 2048)) > 0) {
        /* Recover promptly after a stall (a paused debugger, a slow device):
         * at most 500 ms stay queued. */
        if (SDL_GetQueuedAudioSize(a->device) > (Uint32)a->output.freq * 2) SDL_ClearQueuedAudio(a->device);
        if (SDL_AudioStreamPut(a->stream, samples, (int)(frames * 4)) || !drain(a)) return 0;
        if (frames < 2048) break;
    }
    return 1;
}

uint32_t mc_host_audio_queued(const mc_host_audio *a) { return a->device ? SDL_GetQueuedAudioSize(a->device) : 0; }

void mc_host_audio_close(mc_host_audio *a) {
    if (a->stream) SDL_FreeAudioStream(a->stream);
    if (a->device) SDL_CloseAudioDevice(a->device);
    memset(a, 0, sizeof *a);
}

/* --- microphone input ---------------------------------------------------------- */

static void capture_audio(void *ctx, Uint8 *data, int bytes) {
    mc_host_capture *c = ctx;
    for (int at = 0; at + 4 <= bytes; at += 4) {
        if (c->count == MC_CAPTURE_FRAMES) {
            c->read = (c->read + 1u) % MC_CAPTURE_FRAMES;
            c->count--;
        }
        const unsigned write = (c->read + c->count) % MC_CAPTURE_FRAMES;
        memcpy(c->stereo + write * 2u, data + at, 4);
        c->count++;
    }
}

void mc_host_capture_close(mc_host_capture *c) {
    if (c->device) SDL_CloseAudioDevice(c->device);
    if (c->initialized) SDL_QuitSubSystem(SDL_INIT_AUDIO);
    memset(c, 0, sizeof *c);
}

static void capture_enable(void *ctx, unsigned rate) {
    mc_host_capture *c = ctx;
    if (rate && rate == c->rate) return;
    mc_host_capture_close(c);
    if (!rate) return;
    c->rate = rate;
    if (SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        fprintf(stderr, "Sound input unavailable: %s\n", SDL_GetError());
        return;
    }
    c->initialized = 1;
    SDL_AudioSpec want = {0};
    want.freq = (int)rate; want.format = AUDIO_S16SYS; want.channels = 2; want.samples = 512;
    want.callback = capture_audio; want.userdata = c;
    /* SDL_OpenAudioDevice (SDL2): with no allowed changes, SDL converts
     * the hardware's samples to the requested guest format and rate. */
    c->device = SDL_OpenAudioDevice(NULL, 1, &want, NULL, 0);
    if (c->device) SDL_PauseAudioDevice(c->device, 0);
    else fprintf(stderr, "Sound input unavailable: %s\n", SDL_GetError());
}

static void capture_sample(void *ctx, unsigned rate, int16_t frame[2]) {
    mc_host_capture *c = ctx;
    (void)rate;
    frame[0] = frame[1] = 0;
    if (!c->device) return;
    SDL_LockAudioDevice(c->device);
    if (c->count) {
        memcpy(frame, c->stereo + c->read * 2u, 4);
        c->read = (c->read + 1u) % MC_CAPTURE_FRAMES;
        c->count--;
    }
    SDL_UnlockAudioDevice(c->device);
}

mc_audio_source mc_host_capture_callbacks(mc_host_capture *c) {
    const mc_audio_source source = {.ctx=c, .sample=capture_sample, .enable=capture_enable};
    return source;
}

/* --- WAV recording ------------------------------------------------------------- */

static void le(uint8_t *p, uint32_t v, int n) { for (int i = 0; i < n; i++) p[i] = (uint8_t)(v >> (8 * i)); }

static int wav_header(mc_wav *w) {
    uint8_t h[44];
    memset(h, 0, sizeof h);
    memcpy(h, "RIFF", 4);
    le(h + 4, 36u + w->frames * 4u, 4);
    memcpy(h + 8, "WAVEfmt ", 8);
    le(h + 16, 16, 4);
    le(h + 20, 1, 2);              /* PCM */
    le(h + 22, 2, 2);              /* stereo */
    le(h + 24, w->rate, 4);
    le(h + 28, w->rate * 4u, 4);
    le(h + 32, 4, 2);
    le(h + 34, 16, 2);
    memcpy(h + 36, "data", 4);
    le(h + 40, w->frames * 4u, 4);
    return !fseek(w->file, 0, SEEK_SET) && fwrite(h, 1, sizeof h, w->file) == sizeof h && !fseek(w->file, 0, SEEK_END);
}

int mc_wav_open(mc_wav *w, const char *path) {
    memset(w, 0, sizeof *w);
    w->file = fopen(path, "wb");
    return w->file && wav_header(w);
}

void mc_wav_pump(mc_wav *w, mc_machine *m) {
    int16_t buf[2048 * 2];
    size_t n;
    if (!w->file) return;
    if (!w->rate) w->rate = mc_machine_audio_rate(m);
    while ((n = mc_machine_audio_pull(m, buf, 2048)) > 0) {
        for (size_t i = 0; i < 2 * n; i++) {
            const int v = buf[i] < 0 ? -buf[i] : buf[i];
            if (v > w->peak) w->peak = v;
        }
        /* WAV samples are little-endian. */
        uint8_t out[2048 * 4];
        for (size_t i = 0; i < 2 * n; i++) le(out + 2 * i, (uint16_t)buf[i], 2);
        fwrite(out, 4, n, w->file);
        w->frames += (uint32_t)n;
        if (n < 2048) break;
    }
}

int mc_wav_close(mc_wav *w) {
    if (!w->file) return 1;
    if (!w->rate) w->rate = 48000;   /* a muted, empty capture is still a valid WAV */
    const int ok = wav_header(w) && !ferror(w->file);
    return fclose(w->file) == 0 && ok;
}
