/* Closing the window with emulation.close_action: shutdown: Livadix presses the power key of the Apple Extended Keyboard, as the
 * System's user would, and Return in the shut down dialog it brings up; the
 * window loop then waits for the Macintosh to switch itself off
 * (mc_machine_powered_off). A System that brings up no dialog within
 * SHUTDOWN_DIALOG_MS (System 7.1 ignores the key) leaves the close
 * unanswered, and Livadix quits.
 *
 * The System is observed through its low memory and the Window Manager's
 * records (mc_machine_peek); nothing in the guest is changed. */
#ifndef MC_SHUTDOWN_H
#define MC_SHUTDOWN_H

#include "../core/machine.h"

#include <stdint.h>

typedef struct mc_shutdown {
    int state;
    const char *how;            /* the way being taken, in words */
    uint64_t at;                /* guest time of the next step */
    uint64_t until;             /* guest time the wait for the dialog gives up */
    uint32_t front;             /* the front window when the power key went down */
    uint64_t unmounted;         /* guest time the volume queue was first seen empty (0: not yet) */
} mc_shutdown;

/* Guest time the power key's dialog is waited for. */
#define SHUTDOWN_DIALOG_MS 3000u

/* Whether a System runs that can be asked to shut down: the Process
 * Manager has a current application. */
int mc_shutdown_system_running(mc_machine *m);
/* Whether a System of any age shows its desktop: as above, and the Systems
 * of the 64K ROM (Macintosh 128K and 512K), which the power key cannot shut
 * down. Its pointer can then be placed (machine.h: mc_machine_mouse_to). */
int mc_shutdown_desktop_running(mc_machine *m);
/* Starts the shutdown (the power key goes down); 0 when no System runs (the
 * caller quits at once). */
int mc_shutdown_begin(mc_shutdown *sd, mc_machine *m);
/* After every run of the machine: the next key when its time has come. */
void mc_shutdown_step(mc_shutdown *sd, mc_machine *m);
/* Whether its keys are still going in (the host's keys and mouse wait);
 * afterwards the user answers whatever the System asks. */
int mc_shutdown_injecting(const mc_shutdown *sd);
/* 1 once no shut down dialog came up for the power key: the caller quits. */
int mc_shutdown_unanswered(const mc_shutdown *sd);
/* After every run of the machine while it shuts down: 1 once the System has
 * finished shutting down without switching the machine off (a model
 * without soft power, which shows "It is now safe to switch off your
 * Macintosh", or any model that stops there): the File Manager's volume
 * queue (VCBQHdr, low memory $356) has been empty for SHUTDOWN_FINAL_MS of
 * guest time. While a System runs its startup volume is always mounted;
 * ShutDown unmounts every volume as its last step before it switches the
 * power off or puts up its final alert, and a machine with soft power
 * switches off within a few milliseconds of that, or within seconds: the
 * IIvx sends Egret's power down 2.8 s of guest time after the last volume
 * was unmounted (System 7.1.1, observed). */
int mc_shutdown_final(mc_shutdown *sd, mc_machine *m);
#define SHUTDOWN_FINAL_MS 5000u

#endif /* MC_SHUTDOWN_H */
