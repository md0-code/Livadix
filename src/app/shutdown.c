/* Closing the window: the System shut down by its own Shut Down, asked for
 * with its power key (shutdown.h).
 *
 * Sources: Inside Macintosh: Operating System Utilities and Macintosh
 * Toolbox Essentials, their low-memory summaries (CurApName $910,
 * WindowList $9D6, MenuList $A1C, MBarHeight $BAA, MMU32Bit $CB2) and the
 * window record (windowKind at 108, dialogKind 2). Guide to the Macintosh
 * Family Hardware, 2nd ed., chapter 8: the Apple Extended Keyboard's power
 * key ($7F) and Return ($24). Inside Macintosh: Files, "File Manager" data
 * structures: the volume control block queue (VCBQHdr $356, its qHead at
 * $358).
 *
 * Observed on the Quadra 700: from System 7.5 the power
 * key brings up "Are you sure you want to shut down your computer now?", a
 * dialog in front of the current layer whose default button (Return) is Shut
 * Down; System 7.1 posts no event for the key.
 *
 * Livadix's own code. */
#include "shutdown.h"

#include <string.h>

#define ROM85 0x028Eu           /* negative with the 64K ROM (Inside Macintosh IV, "Compatibility Guidelines") */
#define VCB_QHEAD 0x0358u       /* VCBQHdr ($356): its qHead */
#define CUR_AP_NAME 0x0910u
#define WINDOW_LIST 0x09D6u
#define MENU_LIST 0x0A1Cu
#define MBAR_HEIGHT 0x0BAAu
#define MMU32BIT 0x0CB2u
#define WINDOW_KIND 108u
#define DIALOG_KIND 2u

#define KEY_POWER 0x7Fu
#define KEY_RETURN 0x24u

#define MS(n) ((uint64_t)MC_C15M_HZ * (n) / 1000u)

enum { IDLE, POWER_UP, ALERT, RETURN_DOWN, RETURN_UP, WAIT, UNANSWERED };

static uint32_t peek(mc_machine *m, uint32_t address, unsigned size) {
    uint32_t v = 0;
    return mc_machine_peek(m, address, size, &v) ? v : 0;
}
static uint32_t peek16(mc_machine *m, uint32_t address) {
    return peek(m, address, 1) << 8 | peek(m, address + 1, 1);
}
static uint32_t peek32(mc_machine *m, uint32_t address) {
    return peek16(m, address) << 16 | peek16(m, address + 2);
}
/* A pointer or a master pointer: in 24-bit mode the high byte holds the
 * Memory Manager's flags. */
static uint32_t pointer(mc_machine *m, uint32_t address) {
    const uint32_t p = peek32(m, address);
    return peek(m, MMU32BIT, 1) ? p : p & 0x00FFFFFFu;
}

/* A Pascal string; its length, -1 when unreadable. */
static int pstring(mc_machine *m, uint32_t address, char *out, size_t capacity) {
    uint32_t n = 0;
    if (!mc_machine_peek(m, address, 1, &n)) return -1;
    size_t k = 0;
    for (uint32_t i = 0; i < n; i++) {
        const uint32_t c = peek(m, address + 1 + i, 1);
        if (k + 1 < capacity) out[k++] = (char)c;
    }
    if (capacity) out[k < capacity ? k : capacity - 1] = 0;
    return (int)n;
}

/* The current application's name: 1 to 31 characters, none a control
 * character (while the System starts, the field holds a test pattern or
 * $FF bytes). */
static int application(mc_machine *m, char *name, size_t capacity) {
    const int n = pstring(m, CUR_AP_NAME, name, capacity);
    if (n < 1 || n > 31) return 0;
    for (int i = 0; i < n && name[i]; i++)
        if ((uint8_t)name[i] < 0x20u || (uint8_t)name[i] == 0x7Fu) return 0;
    return 1;
}

int mc_shutdown_system_running(mc_machine *m) {
    char name[64];
    const uint32_t bar = peek16(m, MBAR_HEIGHT);
    return application(m, name, sizeof name) && bar >= 12 && bar <= 64 && pointer(m, MENU_LIST) != 0;
}

int mc_shutdown_desktop_running(mc_machine *m) {
    char name[64];
    if (mc_shutdown_system_running(m)) return 1;
    /* The 64K ROM keeps no MBarHeight: its menu bar is 20 lines. */
    return (peek16(m, ROM85) & 0x8000u) && application(m, name, sizeof name) && pointer(m, MENU_LIST) != 0;
}

int mc_shutdown_final(mc_shutdown *sd, mc_machine *m) {
    const uint64_t now = mc_machine_time(m);
    if (sd->state == IDLE || peek32(m, VCB_QHEAD) != 0) {
        sd->unmounted = 0;
        return 0;
    }
    if (!sd->unmounted) sd->unmounted = now ? now : 1;
    return now - sd->unmounted >= MS(SHUTDOWN_FINAL_MS);
}


int mc_shutdown_begin(mc_shutdown *sd, mc_machine *m) {
    memset(sd, 0, sizeof *sd);
    if (!mc_shutdown_system_running(m)) return 0;
    sd->how = "the power key";
    sd->front = pointer(m, WINDOW_LIST);
    mc_machine_key(m, KEY_POWER, 1);
    sd->state = POWER_UP;
    sd->at = mc_machine_time(m) + MS(100);
    return 1;
}

void mc_shutdown_step(mc_shutdown *sd, mc_machine *m) {
    const uint64_t now = mc_machine_time(m);
    if (sd->state == IDLE || sd->state == WAIT || sd->state == UNANSWERED || now < sd->at) return;
    switch (sd->state) {
    case POWER_UP:
        mc_machine_key(m, KEY_POWER, 0);
        sd->state = ALERT;
        sd->until = now + MS(SHUTDOWN_DIALOG_MS);
        sd->at = now + MS(50);
        return;
    case ALERT: {
        /* The shut down dialog: a new dialog in front. Without it (the
         * System ignores the key, or another dialog holds it) the close
         * quits. */
        const uint32_t front = pointer(m, WINDOW_LIST);
        if (front && front != sd->front && peek16(m, front + WINDOW_KIND) == DIALOG_KIND) {
            sd->state = RETURN_DOWN;
            sd->at = now + MS(300);
        } else if (now >= sd->until) sd->state = UNANSWERED;
        else sd->at = now + MS(50);
        return;
    }
    case RETURN_DOWN:
        mc_machine_key(m, KEY_RETURN, 1);
        sd->state = RETURN_UP;
        sd->at = now + MS(100);
        return;
    case RETURN_UP:
        mc_machine_key(m, KEY_RETURN, 0);
        sd->state = WAIT;
        return;
    default:
        return;
    }
}

int mc_shutdown_injecting(const mc_shutdown *sd) { return sd->state != IDLE && sd->state != WAIT && sd->state != UNANSWERED; }
int mc_shutdown_unanswered(const mc_shutdown *sd) { return sd->state == UNANSWERED; }