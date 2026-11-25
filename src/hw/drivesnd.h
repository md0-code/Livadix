/* Livadix - the sound of the diskette drives, synthesised, from nothing but the
 * drives' own mechanical events (hw.h: mc_dev_drive): every sound is a few
 * damped resonances, a burst of noise and a slow oscillator, summed. The
 * resonances, their decays, their levels and
 * their timing are those found in recordings of an Alps Electric DF354H148F
 * 3.5-inch drive; no sample of a
 * recording is played.
 *
 *  - A step is about 40 ms of sound, most of it between 400 Hz and 1.6 kHz:
 *    a low knock (405 and 553 Hz) under a rasp of noise, the motor's ring
 *    (1.4 and 1.7 kHz) 7 ms later, the strike on the frame's resonance near
 *    811 Hz at 13 ms, and the carriage settling on it at 23 and 32 ms. Each
 *    step is a little different (pitch and level drawn from a fixed pseudo-
 *    random sequence); a step outwards is a little softer, one against the
 *    end stop heavier, and the settling of a step is dropped when the next
 *    step comes first, so a seek is a run of knocks that ends in one settle.
 *  - The motor starts with a low kick and, 43 ms later, the bright click of
 *    the mechanism engaging (2.1 to 2.5 kHz, three times a step's peak). The
 *    head load of a drive with a head-load solenoid is that click at a third
 *    of the level, the unload softer and lower, a hold time after the last
 *    command that used the head (the controller's HUT).
 *  - The running motor is the tones of a 240 Hz series and one near 816 Hz
 *    over a band of noise, swelling with the disk's turn (5 Hz); it is at
 *    speed within a tenth of a second and runs down in pulses in three.
 *  Drive n sits a little to the left (even n) or right (odd) of the middle.
 *  Levels are well below the speaker's: a step peaks at about 4500 of 32768 at
 *  volume 100, the motor's click at about 15000, and a running motor has an
 *  RMS of about 100; the default volume is 50.
 *
 * Time. Events carry guest master-clock timestamps. Livadix renders this
 * source at 48 kHz and mixes it into the machine's DAC stream; the 16-frame
 * event latency and the bounded backlog keep it in step. Muting drops
 * synthesis work while still observing the physical motor state.
 *
 * Deterministic: the same events and the same frames give the same samples,
 * whatever the size of the pulls. No C library math (the sines and decays are
 * summed from series). Portable C11. Livadix's own code. */
#ifndef MC_DRIVESND_H
#define MC_DRIVESND_H

#include <stdint.h>

#include <stddef.h>

enum { MC_DRIVE_MOTOR, MC_DRIVE_STEP, MC_DRIVE_HEAD_LOAD, MC_DRIVE_HEAD_HOLD };

#define MC_DRIVESND_DRIVES 4u           /* diskette drives the source tells apart (drive numbers above it are ignored) */
#define MC_DRIVESND_LATENCY 16u         /* inherited event latency */
#define MC_DRIVESND_BACKLOG 8000u       /* the most a pull hands out; earlier frames are dropped */
#define MC_DRIVESND_QUEUE 512u          /* events kept between pulls (the oldest are played at once beyond it) */
#define MC_DRIVESND_RES 128u            /* damped resonances sounding at once */
#define MC_DRIVESND_BURSTS 48u          /* noise bursts sounding at once */
#define MC_DRIVESND_TABLE 1024u         /* the oscillator's sine table */

typedef struct mc_drivesnd_item {
    uint64_t t;
    uint32_t a, b;
    uint8_t drive, kind, dead;
} mc_drivesnd_item;

typedef struct mc_drivesnd_res {        /* a damped resonance: x(n) = A r^n cos(n w) as a rotating phasor */
    double x, y, c, s, r, left, right;
    uint8_t live;
} mc_drivesnd_res;

typedef struct mc_drivesnd_burst {      /* a decaying burst of low-passed noise */
    double env, decay, lp, k, left, right;
    uint8_t live;
} mc_drivesnd_burst;

typedef struct mc_drivesnd_spin {       /* a diskette motor: its speed fraction (0 to 1) and its oscillators */
    double level, target, lp1, lp2;
    uint32_t phase[4];                  /* the 240 Hz series, the disk's turn, the 816 Hz tone, the run-down's pulses */
} mc_drivesnd_spin;

typedef struct mc_drivesnd {
    int enabled;
    uint8_t physical_motor;             /* retained while muted */
    int mix_have;
    uint32_t mix_rate, mix_phase;
    int16_t mix_sample[2];
    double gain;                        /* volume / 100 */
    uint32_t clock_hz, rate;
    uint32_t num, den;                  /* frames per master-clock tick, reduced */
    double inc;                         /* a 32-bit phase's increment for 1 Hz */
    uint64_t done;                      /* the next frame to be produced */
    uint32_t rng;                       /* the pseudo-random sequence */
    mc_drivesnd_item q[MC_DRIVESND_QUEUE];
    unsigned qh, qn;                    /* the queue's first and one past its last, sorted by time */
    uint64_t next_frame;                /* the first queued event's frame; UINT64_MAX when none */
    uint64_t hold_t[MC_DRIVESND_DRIVES];/* the pending head unload's time, UINT64_MAX when none */
    uint8_t loaded;                     /* bit n: drive n's head is loaded (as far as the events say) */
    uint8_t motor;                      /* bit n: drive n's motor is on (as far as the events say) */
    mc_drivesnd_spin spin[MC_DRIVESND_DRIVES];
    uint8_t spin_mask;                  /* the drives whose motor sounds (bit n: drive n) */
    mc_drivesnd_res res[MC_DRIVESND_RES];
    mc_drivesnd_burst burst[MC_DRIVESND_BURSTS];
    unsigned res_next, burst_next, nres, nburst;
    double atk, rel;                    /* the motor's smoothing per sample */
    double table[MC_DRIVESND_TABLE + 1u];
} mc_drivesnd;

/* Sets the source up for a master clock of `clock_hz` and output frames at `rate` Hz, disabled, with
 * the first frame at guest time 0 (mc_drivesnd_reset moves it). */
void mc_drivesnd_init(mc_drivesnd *s, uint32_t clock_hz, uint32_t rate);
/* Switches the sound on or off and sets its volume (0 to 100 percent; 0 is off). A change of the state
 * clears everything that sounds or waits. Motor events continue to update physical state while muted. */
void mc_drivesnd_set(mc_drivesnd *s, int enabled, unsigned volume);
static inline int mc_drivesnd_enabled(const mc_drivesnd *s) { return s->enabled; }
/* Clears everything that sounds or waits (a reset, a state load) and puts the next frame at guest time `t`. */
void mc_drivesnd_reset(mc_drivesnd *s, uint64_t t);
/* Mechanical observer; `ctx` is the source. Muted sources retain motor state. */
void mc_drivesnd_event(void *ctx, unsigned drive, unsigned event, uint64_t t, uint32_t a, uint32_t b);
/* The index of the next frame the render call will produce. */
static inline uint64_t mc_drivesnd_position(const mc_drivesnd *s) { return s->done; }
/* Produces up to `frames` stereo frames, in order, from where
 * the last call stopped, only the frames final at guest time `until`: fewer than asked when the machine has
 * not run that far. Returns the frames written (2 int16_t each). _add adds them, saturating, to what the
 * buffer holds instead of overwriting it. A disabled source writes silence (_add:
 * leaves the buffer) and keeps its position. */
size_t mc_drivesnd_render(mc_drivesnd *s, uint64_t until, int16_t *stereo, size_t frames);
size_t mc_drivesnd_render_add(mc_drivesnd *s, uint64_t until, int16_t *stereo, size_t frames);
/* Something sounds or is waiting to (tests, and the idle shortcut). */
int mc_drivesnd_busy(const mc_drivesnd *s);
/* Mix the fixed 48 kHz source into a machine DAC stream at any rate. */
void mc_drivesnd_mix_add(mc_drivesnd *s, uint64_t until, int16_t *stereo, size_t frames, uint32_t rate);

#endif /* MC_DRIVESND_H */
