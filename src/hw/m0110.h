/* Livadix — the keyboards of the Macintosh 128K, 512K and Plus: the M0110 and
 * the M0110A (with its numeric keypad), on the VIA's shift register.
 *
 * Sources: Guide to the Macintosh Family Hardware, 2nd ed., chapter 14
 * ("The Keyboard": the keyboard drives the clock on VIA CB1 and the data on
 * CB2; the Macintosh sends a command with the shift register shifting out
 * under the external clock, then shifts the reply in under the same clock;
 * the commands Inquiry $10, Instant $14, Model Number $16 and Test $36; the
 * replies: a key transition, Null $7B, the model byte, ACK $7D or NAK $77;
 * a key transition is the key code shifted left once with bit 0 set, bit 7
 * set when the key goes up; the keypad's keys come after the prefix $79;
 * Inquiry waits up to a quarter of a second for a transition); Inside
 * Macintosh, volume III, "Keyboard" (the key codes). Observed in ROM
 * 4D1F8172: the keyboard interrupt at ROM offset
 * $25E4 sends the next Inquiry after Null, Instant after the prefix $79,
 * adds $40 to the code after it, and takes the reply to Model Number at
 * start; the command is written to the shift register in mode 7 and the
 * reply read in mode 3, after a short delay loop.
 *
 * Livadix's own code. The keyboard is a device of its own: after the
 * Macintosh writes the shift register in mode 7 (mc_m0110_sr_written), it
 * clocks the command out a few milliseconds later and clocks its reply in
 * once the VIA shifts in under the external clock (mode 3), through
 * mc_via_shift8. Keys come in as ADB key codes (the Apple Extended
 * Keyboard's, as the shell sends them) and are mapped to the M0110's or
 * the M0110A's: Control (which neither has) as Command; the right-hand
 * modifiers as the left ones; on the M0110A the arrows and the keypad
 * after $79, its *, /, = and + as Shift with the arrows' codes, as the
 * keypad sends them; keys the keyboard lacks are dropped. */
#ifndef MC_M0110_H
#define MC_M0110_H

#include "hw.h"
#include "via.h"

#include <stdint.h>

enum { MC_M0110 = 0, MC_M0110A = 1 };

/* Commands and replies (Guide, chapter 14). */
enum {
    MC_M0110_INQUIRY = 0x10, MC_M0110_INSTANT = 0x14, MC_M0110_MODEL = 0x16, MC_M0110_TEST = 0x36,
    MC_M0110_NULL = 0x7B, MC_M0110_KEYPAD = 0x79, MC_M0110_ACK = 0x7D, MC_M0110_NAK = 0x77
};

#define MC_M0110_QUEUE 64

typedef struct mc_m0110 {
    mc_dev dev;
    mc_via *via;                /* its shift register: CB1 clock, CB2 data */
    uint8_t model;              /* MC_M0110, MC_M0110A */
    uint8_t queue[MC_M0110_QUEUE];  /* transition bytes not yet sent */
    unsigned head, count;
    uint8_t state;              /* what the next event does */
    uint8_t reply;              /* the reply waiting for mode 3 */
    uint8_t shift;              /* the host's Shift is down (the keypad's synthesized Shift) */
    uint64_t inquiry_end;       /* Inquiry: the Null reply's time */
    uint8_t last_command;
} mc_m0110;

void mc_m0110_init(mc_m0110 *k, const mc_hw_host *host, const char *name, mc_via *via, int model);
extern const mc_dev_ops mc_m0110_ops;
/* The Macintosh wrote the VIA's shift register (call after the write). */
void mc_m0110_sr_written(mc_m0110 *k);
/* A key went down or up (an ADB key code). 0 when the queue is full. */
int mc_m0110_key(mc_m0110 *k, uint8_t adb_code, int down);
/* The model byte the keyboard returns to Model Number: bit 0 set, the
 * model in bits 3-1 (1 the M0110, 5 the M0110A... Guide, chapter 14). */
uint8_t mc_m0110_model_byte(const mc_m0110 *k);

#endif
