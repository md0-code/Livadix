/* Livadix — the quadrature mouse of the Macintosh 128K, 512K and Plus.
 *
 * Sources: Guide to the Macintosh Family Hardware, 2nd ed., chapter 14
 * ("The Mouse": two pairs of quadrature signals, X1/X2 and Y1/Y2; X1 and Y1
 * on the SCC's DCDA and DCDB inputs, whose changes interrupt, X2 and Y2 on
 * VIA port B bits 4 and 5, read to tell the direction; the button on VIA
 * port B bit 3, low while pressed). Observed in ROM 4D1F8172: the external/status handlers at ROM offsets $1BD8
 * (channel A) and $1C00 (channel B) compare RR0's DCD bit with PB4 and
 * PB5: DCDA different from PB4 adds one to MTemp's h, DCDB equal to PB5
 * one to its v.
 *
 * Livadix's own code. Host motion becomes steps, one per axis every 500 us
 * (a quick hand's rate): for each, the second signal is set, then the
 * first toggles, so the handler finds the direction already there. A
 * step waits while the machine says its axis's last edge is not taken
 * yet (`ready`: the SCC's external/status latch still closed, as while the
 * floppy driver masks interrupts), so motion is late rather than lost. Lines
 * (state = level): MC_QUADMOUSE_LINE_X1, _X2, _Y1, _Y2, _BUTTON (1 =
 * pressed). */
#ifndef MC_QUADMOUSE_H
#define MC_QUADMOUSE_H

#include "hw.h"

#include <stdint.h>

enum { MC_QUADMOUSE_LINE_X1, MC_QUADMOUSE_LINE_X2, MC_QUADMOUSE_LINE_Y1, MC_QUADMOUSE_LINE_Y2, MC_QUADMOUSE_LINE_BUTTON };

typedef struct mc_quadmouse {
    mc_dev dev;
    int32_t dx, dy;             /* steps still to make (right, down positive) */
    uint8_t x1, x2, y1, y2, button;
    uint8_t armed;
    /* Optional: 1 when axis 0 (X) or 1 (Y) may step. */
    int (*ready)(void *ctx, int axis);
    void *ready_ctx;
} mc_quadmouse;

void mc_quadmouse_init(mc_quadmouse *m, const mc_hw_host *host, const char *name);
extern const mc_dev_ops mc_quadmouse_ops;
/* Host motion (pixels, one step each) and the button. 0 when too much
 * motion is waiting. */
int mc_quadmouse_move(mc_quadmouse *m, int dx, int dy, int button);
/* The step interval in C15M periods. */
#define MC_QUADMOUSE_STEP ((uint64_t)MC_C15M_HZ / 2000u)

#endif
