/* The window (src/app): the machine paced to wall time, its screen through
 * the presenter, its sound, the keyboard as ADB keys, the captured mouse,
 * media dropped on the window, and the configuration window over the
 * screen (config_app.c): the Windows key opens it and pauses the
 * Macintosh; closing it applies the changes, starting the Macintosh again
 * when they need it. */
#include "desktop.h"
#ifndef _WIN32
#include "livadix_icon.h"
#endif
#include "livadix_logo.h"
#include "mc_version.h"
#include "shutdown.h"
#include "config_app.h"
#include "config_host.h"
#include "../core/profile.h"
#include "../core/romdb.h"
#include "../platform/audio.h"
#include "../platform/serial.h"
#include "../platform/overlay.h"
#include "../platform/video.h"
#include "../net/net.h"

#include <SDL_syswm.h>
#include <SDL_system.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <signal.h>
#endif

#define TITLE_FREE "Livadix - click to capture the mouse"
#define TITLE_CAPTURED "Livadix - Ctrl+Alt releases the mouse"
#define TITLE_STOPPED "Livadix - stopped: click for the configuration"
#ifdef _WIN32
#define TITLE_CONFIG "Livadix - the Windows key goes back to the Macintosh"
#else
#define TITLE_CONFIG "Livadix - the Super key goes back to the Macintosh"
#endif
/* Pacing: lags up to 50 ms of wall time are caught up; a longer one (a
 * stalled host) is dropped, so the Macintosh never races to catch up. */
#define CATCH_UP_MS 50
/* The time a present takes is caught up in full up to this (desktop.c,
 * the pacing). */
#define PRESENT_CATCH_UP_MS 500
/* While the Macintosh catches up, a frame is still shown at least every
 * this many frames (desktop.c, the pacing). */
#define PRESENT_MAX_FRAMES 4
/* The configuration window's smallest canvas (its layout's size). */
#define CONFIG_W 820
#define CONFIG_H 600

typedef struct shell {
    mc_machine *m;                     /* NULL: none runs (it could not start) */
    int owns;                          /* the session built it: destroyed here */
    mc_config live;                    /* the settings in force */
    mc_config started;                 /* what the machine was built with */
    mc_config opened;                  /* the running profile as the configuration window opened on it */
    char profile[MC_CONFIG_NAME];
    int has_profile;
    const mc_desktop_options *options;
    const mc_desktop_session *session;
    mc_video *video;
    int hidden;
    mc_osd osd;
    /* keys the machine has not taken yet */
    uint8_t pending[1024];
    unsigned read, write;
    uint8_t pressed[128];
    unsigned buttons;
    /* A game controller on a console (the Pippin): its buttons are the keys
     * and the mouse button the console's own controller takes (pad_event). */
    SDL_GameController *pad;
    /* A close request with emulation.close_action: shutdown (shutdown.h):
     * the guest is shutting down since close_ms (host) and close_time
     * (guest). */
    mc_shutdown sd;
    int closing;
    uint32_t close_ms;
    uint64_t close_time;
    char error[256];
    /* The machine's host ends. */
    mc_host_audio audio;
    mc_host_capture microphone;
    mc_wav wav;
    mc_host_serial serial;
    mc_serial_host serial_host;
    mc_net *net;
    unsigned width, height;            /* the screen shown */
    uint32_t *pixels;                  /* the frame presented */
    uint32_t *overlay;                 /* the configuration window over it (mc_video_set_overlay) */
    /* The configuration window. */
    cfg_app *app;
    int config_open;
    int save_failed;
    int quit;                          /* the configuration window quits Livadix */
    int stop;                          /* its Stop: the Macintosh shuts down and stays off */
    int stopping;                      /* that shutdown runs: its end stops instead of quitting */
    int restart_queued;                /* and then the profile starts again (restart_config) */
    uint32_t *logo;                    /* the logo screen (none runs), logo_w x logo_h */
    int logo_w, logo_h, logo_shown;    /* logo_shown: it is in the window */
    int asked;                         /* unsaved changes were asked about when closing */
    int resume;                        /* it closed: apply the changes */
    int restart;                       /* Start: restart_config with restart_profile */
    mc_config restart_config;
    mc_config built;                   /* the running profile as the window first opened on this Macintosh: what a
                                        * change that needs a new start is measured from */
    int built_set;
    unsigned media_mark;               /* cfg_app media_chosen when the window opened */
    char restart_profile[MC_CONFIG_NAME];
    int restart_has_profile;
    int window_w, window_h, window_resized; /* the window's size before the configuration window enlarged it */
    Uint32 last_click;
    /* A Remote Desktop session points by position where the guest can take
     * one, with no client pointer warp or confinement: 2 a Classic System's
     * pointer, placed through its cursor globals; 1 the guest's absolute
     * device (the G3's USB pointer under Mac OS X); 0 neither, native raw
     * input. */
    SDL_Cursor *remote_cursor;
    int absolute_mouse, absolute_x, absolute_y;
    Uint32 absolute_checked;           /* when the way of pointing was last looked at */
    /* Way 2: the button's changes wait until the System's pointer is where
     * the host's is, and stay apart so a press and release that arrive
     * together remain a click. */
    uint8_t clicks[8];
    unsigned click_count;
    uint64_t click_at, click_until;    /* guest time: the next change's earliest, and its latest wait for the pointer */
    FILE *mouse_trace;
#ifdef _WIN32
    HWND mouse_trace_window;
#else
    void *mouse_trace_window;
#endif
    unsigned mouse_trace_lines;
    /* checks: the script */
    cfg_script script;
    uint64_t script_wait;              /* frames to run before its next command */
} shell;

/* LIVADIX_STATS=FILE: once per wall second, a line of what the window loop
 * did (guest time against wall time, presents and their cost, frames
 * skipped while catching up, time dropped, pictures that changed), for
 * diagnosing pacing and presentation on a real display. */
typedef struct stats {
    FILE *f;
    uint64_t t0, last, guest0, skip_at;
    unsigned presents, changed, skipped;
    uint64_t convert, present, present_max, dropped;
    uint32_t *previous;
    size_t previous_n;
} stats;

static void stats_tick(stats *st, mc_machine *m, uint64_t now, uint64_t freq) {
    if (!st->f || !m || now - st->last < freq) return;
    const double wall = (double)(now - st->last) / (double)freq;
    const uint64_t guest = mc_machine_time(m);
    const double ms = 1000.0 / (double)freq;
    fprintf(st->f, "t=%.1f guest=%.1f%% presents=%u changed=%u convert=%.2fms present=%.2fms max=%.2fms skipped=%u dropped=%.1fms\n",
            (double)(now - st->t0) / (double)freq, 100.0 * (double)(guest - st->guest0) / MC_C15M_HZ / wall, st->presents,
            st->changed, st->presents ? (double)st->convert * ms / st->presents : 0.0,
            st->presents ? (double)st->present * ms / st->presents : 0.0, (double)st->present_max * ms, st->skipped,
            (double)st->dropped * 1000.0 / MC_C15M_HZ);
    fflush(st->f);
    st->last = now;
    st->guest0 = guest;
    st->presents = st->changed = st->skipped = 0;
    st->convert = st->present = st->present_max = st->dropped = 0;
}

/* The console window's close button and Ctrl+Break (Ctrl+C reaches SDL as
 * SIGINT): a close request. Windows ends the process about five seconds
 * after a close, so a shutdown not done by then is cut short with a second
 * request, which still closes the images. */
#ifdef _WIN32
static volatile LONG console_open;
static BOOL WINAPI console_event(DWORD type) {
    if ((type != CTRL_CLOSE_EVENT && type != CTRL_BREAK_EVENT) || !console_open) return FALSE;
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = SDL_QUIT;
    SDL_PushEvent(&e);
    if (type == CTRL_CLOSE_EVENT) {
        Sleep(3500);
        if (console_open) SDL_PushEvent(&e);
        Sleep(1500);
    }
    return TRUE;
}
#else
/* On Linux SDL itself turns SIGINT and SIGTERM into SDL_QUIT. The terminal
 * closing (SIGHUP) is a close request too: the signal handler only counts it
 * and SDL's timer thread posts the request. */
static volatile int console_open;
static volatile sig_atomic_t hangups;
static struct sigaction hangup_before;
static SDL_TimerID hangup_timer;
static void hangup_signal(int sig) { (void)sig; ++hangups; }
static Uint32 SDLCALL hangup_poll(Uint32 interval, void *ctx) {
    static sig_atomic_t posted;
    (void)ctx;
    while (posted != hangups) {
        SDL_Event e;
        memset(&e, 0, sizeof e);
        e.type = SDL_QUIT;
        SDL_PushEvent(&e);
        ++posted;
    }
    return interval;
}
static void console_handler(int on) {
    if (on) {
        struct sigaction action;
        memset(&action, 0, sizeof action);
        action.sa_handler = hangup_signal;
        sigemptyset(&action.sa_mask);
        action.sa_flags = SA_RESTART;
        sigaction(SIGHUP, &action, &hangup_before);
        hangup_timer = SDL_AddTimer(100, hangup_poll, NULL);
    } else {
        if (hangup_timer) SDL_RemoveTimer(hangup_timer);
        hangup_timer = 0;
        sigaction(SIGHUP, &hangup_before, NULL);
    }
}
#endif

/* The Windows keys while Livadix's window has the focus: they open and close
 * the configuration window, and neither the Start menu nor the Macintosh
 * sees them (a low-level keyboard hook takes them before Windows does). On
 * Linux an SDL event watch takes the Super keys the window receives (it
 * receives them from the desktop while the mouse is captured: capture()
 * grabs the keyboard then). */
#ifdef _WIN32
static HHOOK keyboard_hook;
static HWND hook_window;
#else
static SDL_Window *hook_window;
#endif
static Uint32 config_event;
static int windows_key_down;
#ifdef _WIN32
static LRESULT CALLBACK windows_key(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION) {
        const KBDLLHOOKSTRUCT *k = (const KBDLLHOOKSTRUCT *)lp;
        if ((k->vkCode == VK_LWIN || k->vkCode == VK_RWIN) && hook_window && GetForegroundWindow() == hook_window) {
            const int down = wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN;
            if (down && !windows_key_down) {
                SDL_Event e;
                memset(&e, 0, sizeof e);
                e.type = config_event;
                SDL_PushEvent(&e);
            }
            windows_key_down = down;
            return 1;
        }
    }
    return CallNextHookEx(keyboard_hook, code, wp, lp);
}
static void hook_install(mc_video *video) {
    SDL_SysWMinfo wm;
    SDL_VERSION(&wm.version);
    if (!SDL_GetWindowWMInfo(mc_video_window(video), &wm)) return;
    hook_window = wm.info.win.window;
    keyboard_hook = SetWindowsHookExW(WH_KEYBOARD_LL, windows_key, GetModuleHandleW(NULL), 0);
}
static void hook_remove(void) {
    if (keyboard_hook) UnhookWindowsHookEx(keyboard_hook);
    keyboard_hook = NULL;
    hook_window = NULL;
}
#else
/* A watch, not a filter: setting a filter discards the events pending. The
 * Super keys go on to event(), which gives them to no one (keymap.c). */
static int SDLCALL windows_key(void *ctx, SDL_Event *e) {
    (void)ctx;
    if ((e->type == SDL_KEYDOWN || e->type == SDL_KEYUP) && hook_window &&
        (e->key.keysym.scancode == SDL_SCANCODE_LGUI || e->key.keysym.scancode == SDL_SCANCODE_RGUI)) {
        const int down = e->type == SDL_KEYDOWN;
        if (down && !windows_key_down) {
            SDL_Event open;
            memset(&open, 0, sizeof open);
            open.type = config_event;
            SDL_PeepEvents(&open, 1, SDL_ADDEVENT, 0, 0);
        }
        windows_key_down = down;
    }
    return 0;
}
static void hook_install(mc_video *video) {
    hook_window = mc_video_window(video);
    SDL_AddEventWatch(windows_key, NULL);
}
static void hook_remove(void) {
    if (hook_window) SDL_DelEventWatch(windows_key, NULL);
    hook_window = NULL;
}
#endif

/* A close request (the window's close button, Alt+F4, Ctrl+C, the console
 * closed): 0 ends the run now. With close_action shutdown and a System
 * running, the Macintosh is asked to shut down and the run goes on until it
 * switches itself off; a second request, or close_timeout seconds, end it
 * at once. */
static int close_request(shell *s) {
    if (!s->m) return 0;
    if (s->closing) {
        printf("Close: quit at once (closed again)\n");
        return 0;
    }
    if (s->live.close_action != MC_CLOSE_SHUTDOWN) return 0;
    if (!mc_shutdown_begin(&s->sd, s->m)) {
        printf("Close: no System is running; quit at once\n");
        return 0;
    }
    s->closing = 1;
    s->close_ms = SDL_GetTicks();
    s->close_time = mc_machine_time(s->m);
    printf("Close: the Macintosh shuts down (%s); closing again quits at once\n", s->sd.how);
    fflush(stdout);
    return 1;
}

/* SM_REMOTESESSION alone can miss accelerated RDP sessions. Microsoft,
 * "Detecting the Remote Desktop Services environment": compare the process
 * session with Terminal Server's GlassSessionId as well. */
#ifndef _WIN32
/* No remote-session pointer mode on Linux: the pointer is captured as SDL does. */
static int remote_session(void) { return 0; }
#else
static int remote_session(void) {
    if (GetSystemMetrics(SM_REMOTESESSION)) return 1;
    DWORD console = 0, bytes = sizeof console, session = 0;
    return RegGetValueW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Terminal Server",
                        L"GlassSessionId", RRF_RT_REG_DWORD, NULL, &console, &bytes) == ERROR_SUCCESS &&
           ProcessIdToSessionId(GetCurrentProcessId(), &session) && session != console;
}
#endif

/* Opt-in input diagnosis. Observe Windows messages before SDL handles
 * them, then record SDL's delivered deltas and capture/cursor state. No
 * keyboard messages or typed text are recorded. */
#ifndef _WIN32
/* Windows only (LIVADIX_MOUSE_TRACE reads Windows messages): nothing on Linux. */
static void mouse_trace_state(shell *s, const char *source, int x, int y, unsigned detail) {
    (void)s;
    (void)source;
    (void)x;
    (void)y;
    (void)detail;
}
static void mouse_trace_open(shell *s) { (void)s; }
static void mouse_trace_close(shell *s) { (void)s; }
#else
static void mouse_trace_state(shell *s, const char *source, int x, int y, unsigned detail) {
    if (!s->mouse_trace || s->mouse_trace_lines >= 20000u) return;
    CURSORINFO ci = {sizeof ci};
    RECT clip = {0};
    const int cursor_ok = GetCursorInfo(&ci), clip_ok = GetClipCursor(&clip);
    SDL_Window *window = mc_video_window(s->video);
    fprintf(s->mouse_trace,
            "t=%u source=%s x=%d y=%d detail=%08X relative=%d flags=%08X config=%d "
            "mouse_focus=%d key_focus=%d foreground=%d capture=%d cursor_ok=%d cursor=%p show=%lu "
            "position=%ld,%ld clip_ok=%d clip=%ld,%ld,%ld,%ld\n",
            SDL_GetTicks(), source, x, y, detail, (int)SDL_GetRelativeMouseMode(), SDL_GetWindowFlags(window), s->config_open,
            SDL_GetMouseFocus() == window, SDL_GetKeyboardFocus() == window,
            GetForegroundWindow() == s->mouse_trace_window, GetCapture() == s->mouse_trace_window,
            cursor_ok, (void *)ci.hCursor, (unsigned long)ci.flags, ci.ptScreenPos.x, ci.ptScreenPos.y,
            clip_ok, clip.left, clip.top, clip.right, clip.bottom);
    ++s->mouse_trace_lines;
    if ((s->mouse_trace_lines & 31u) == 0 || strcmp(source, "WM_INPUT")) fflush(s->mouse_trace);
}
static void SDLCALL mouse_trace_message(void *ctx, void *window, unsigned message, Uint64 wp, Sint64 lp) {
    shell *s = ctx;
    (void)wp;
    if (!s->mouse_trace || window != s->mouse_trace_window || s->mouse_trace_lines >= 20000u) return;
    if (message == WM_INPUT) {
        RAWINPUT raw;
        UINT size = sizeof raw;
        const UINT read = GetRawInputData((HRAWINPUT)(intptr_t)lp, RID_INPUT, &raw, &size, sizeof(RAWINPUTHEADER));
        if (read != (UINT)-1 && read >= sizeof(RAWINPUTHEADER) + sizeof(RAWMOUSE) && raw.header.dwType == RIM_TYPEMOUSE)
            mouse_trace_state(s, "WM_INPUT", (int)raw.data.mouse.lLastX, (int)raw.data.mouse.lLastY,
                              (unsigned)raw.data.mouse.usFlags << 16 | raw.data.mouse.usButtonFlags);
    } else if (message == WM_MOUSEMOVE) {
        mouse_trace_state(s, "WM_MOUSEMOVE", (int)(short)LOWORD(lp), (int)(short)HIWORD(lp), 0);
    } else if (message == WM_SETCURSOR || message == WM_MOUSELEAVE || message == WM_SETFOCUS || message == WM_KILLFOCUS ||
               message == WM_ACTIVATE || message == WM_CAPTURECHANGED || message == WM_MOUSEACTIVATE) {
        mouse_trace_state(s, "WM_STATE", 0, 0, message);
    }
}
static void mouse_trace_open(shell *s) {
    const char *path = getenv("LIVADIX_MOUSE_TRACE");
    if (!path || !path[0]) return;
    SDL_SysWMinfo wm;
    SDL_VERSION(&wm.version);
    if (!SDL_GetWindowWMInfo(mc_video_window(s->video), &wm)) return;
    s->mouse_trace = fopen(path, "w");
    if (!s->mouse_trace) { fprintf(stderr, "Cannot open the mouse trace: %s\n", path); return; }
    s->mouse_trace_window = wm.info.win.window;
    RECT rect = {0};
    GetClientRect(s->mouse_trace_window, &rect);
    const char *visible = getenv("LIVADIX_MOUSE_VISIBLE");
    fprintf(s->mouse_trace, "mouse-trace-v1 version=%s pid=%lu cursor_policy=%s remote=%d metric_remote=%d screen=%dx%d virtual=%d,%d,%d,%d client=%ldx%ld\n",
            MC_VERSION, (unsigned long)GetCurrentProcessId(), visible && !strcmp(visible, "1") ? "visible-control" : "transparent",
            remote_session(), GetSystemMetrics(SM_REMOTESESSION),
            GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN), GetSystemMetrics(SM_XVIRTUALSCREEN),
            GetSystemMetrics(SM_YVIRTUALSCREEN), GetSystemMetrics(SM_CXVIRTUALSCREEN), GetSystemMetrics(SM_CYVIRTUALSCREEN),
            rect.right, rect.bottom);
    mouse_trace_state(s, "OPEN", 0, 0, 0);
    SDL_SetWindowsMessageHook(mouse_trace_message, s);
}
static void mouse_trace_close(shell *s) {
    if (!s->mouse_trace) return;
    SDL_SetWindowsMessageHook(NULL, NULL);
    mouse_trace_state(s, "CLOSE", 0, 0, 0);
    fclose(s->mouse_trace);
    s->mouse_trace = NULL;
}
#endif

/* How a Remote Desktop session points at this guest (shell.absolute_mouse).
 * A Classic System comes first: Mac OS 9 configures the G3's USB pointer
 * too. */
static int absolute_mode(shell *s) {
    if (!s->m) return 0;
    /* Livadix's own desktop (the Built-in machine) keeps the same globals. */
    if (mc_model_builtin(mc_model_entry(s->live.model)) || mc_shutdown_desktop_running(s->m)) return 2;
    return mc_machine_absolute_ready(s->m) ? 1 : 0;
}
/* Captured in a Remote Desktop session: whether the way of pointing has
 * changed (a System started or ended, the USB pointer was configured).
 * Looked at four times a second: it reads the guest's memory. */
static int pointing_changed(shell *s) {
    if (!s->absolute_mouse && !SDL_GetRelativeMouseMode()) return 0;
    const Uint32 now = SDL_GetTicks();
    if (now - s->absolute_checked < 250) return 0;
    s->absolute_checked = now;
    return remote_session() && absolute_mode(s) != s->absolute_mouse;
}
/* The guest's pointer to where the host's is; 0 when the guest no longer
 * takes positions this way. */
static int absolute_move(shell *s) {
    if (s->absolute_mouse == 2) return mc_machine_mouse_to(s->m, s->absolute_x, s->absolute_y);
    return mc_machine_absolute_mouse(s->m, s->absolute_x, s->absolute_y, s->buttons != 0);
}
/* Way 2: the Macintosh's button, down or up, once its pointer has arrived. */
#define CLICK_APART (MC_C15M_HZ / 30u)      /* two frames between changes: the ADB poll sees each */
#define CLICK_WAIT (MC_C15M_HZ / 20u)       /* the cursor task moves the pointer at a vertical retrace */
static void click_push(shell *s) {
    const uint8_t down = s->buttons != 0;
    if (s->click_count && s->clicks[s->click_count - 1] == down) return;
    if (s->click_count == sizeof s->clicks) s->click_count--;
    if (!s->click_count) s->click_until = mc_machine_time(s->m) + CLICK_WAIT;
    s->clicks[s->click_count++] = down;
}
static void click_step(shell *s) {
    if (!s->click_count) return;
    const uint64_t now = mc_machine_time(s->m);
    if (now < s->click_at) return;
    uint32_t placing = 0;                   /* CrsrNew ($8CE): cleared once the cursor task has taken the position */
    if (now < s->click_until && mc_machine_peek(s->m, 0x8CE, 1, &placing) && placing) return;
    if (!mc_machine_mouse(s->m, 0, 0, s->clicks[0])) return;
    memmove(s->clicks, s->clicks + 1, --s->click_count);
    s->click_at = now + CLICK_APART;
    s->click_until = s->click_at + CLICK_WAIT;
}

static int queue_key(shell *s, uint8_t code, int down);
static int capture(shell *s, int on) {
    if (s->hidden) return 1;
    const int was = s->absolute_mouse;
    s->click_count = 0;
    if (!on && was == 1 && s->m)
        mc_machine_absolute_mouse(s->m, s->absolute_x, s->absolute_y, 0);
    if (!on && s->buttons) {
        /* A later button-up arrives after capture is gone. Release here
         * so Ctrl+Alt, a file chooser or focus loss cannot leave a drag. */
        for (unsigned i = 0; i < 2; ++i)
            if ((s->buttons & (1u << i)) && s->live.mouse[i] == MC_MOUSE_CONTROL_CLICK && !s->pressed[0x36])
                queue_key(s, 0x36, 0);
        s->buttons = 0;
        if (s->m) mc_machine_mouse(s->m, 0, 0, 0);
    }
    const int remote = on && remote_session();
    s->absolute_mouse = remote ? absolute_mode(s) : 0;
    s->absolute_checked = SDL_GetTicks();
    /* The button follows the pointer from the USB device to the ADB mouse and back. */
    if (on && was == 1 && s->absolute_mouse != 1 && s->m) mc_machine_absolute_mouse(s->m, s->absolute_x, s->absolute_y, 0);
    if (s->absolute_mouse == 1 && s->buttons) mc_machine_mouse(s->m, 0, 0, 0);
    const char *visible_setting = getenv("LIVADIX_MOUSE_VISIBLE");
    /* A traced control run can keep the system arrow visible, isolating
     * RDP pointer-position updates from transparent cursor handling. */
    const int visible_control = remote && s->mouse_trace && visible_setting && !strcmp(visible_setting, "1");
    if (remote && !s->remote_cursor) {
        /* The all-zero bitmap creates a fully transparent cursor.
         * Keep a real cursor selected: RDP can ignore warps for NULL, and
         * its client must receive the empty shape instead of an old arrow. */
        const Uint8 blank[32] = {0};
        s->remote_cursor = SDL_CreateCursor(blank, blank, 16, 16, 0, 0);
        if (!s->remote_cursor) {
            snprintf(s->error, sizeof s->error, "Cannot create the Remote Desktop cursor: %s", SDL_GetError());
            return 0;
        }
    }
    SDL_SetRelativeMouseMode(SDL_FALSE);
    SDL_SetWindowGrab(mc_video_window(s->video), SDL_FALSE);
    /* SDL's warp mode bypasses its Windows RDP raw-input handler. Keep
     * that handler active: it discards synthetic jumps and wobbles its
     * recenter target so RDP does not discard repeated identical warps. */
    SDL_SetHintWithPriority(SDL_HINT_MOUSE_RELATIVE_MODE_WARP, "0", SDL_HINT_OVERRIDE);
    /* SDL documents centered confinement for RDP. Full-window clipping
     * let the absolute pointer reach the client bar; the recorded stream
     * then repeatedly hit SDL's top-edge recenter/drop path. */
    SDL_SetHintWithPriority(SDL_HINT_MOUSE_RELATIVE_MODE_CENTER, "1", SDL_HINT_OVERRIDE);
    SDL_SetHintWithPriority(SDL_HINT_MOUSE_RELATIVE_CURSOR_VISIBLE, remote ? "1" : "0", SDL_HINT_OVERRIDE);
    SDL_SetCursor(remote && !visible_control ? s->remote_cursor : SDL_GetDefaultCursor());
    SDL_ShowCursor(SDL_ENABLE);
    if (SDL_SetRelativeMouseMode(on && !s->absolute_mouse ? SDL_TRUE : SDL_FALSE) != 0) {
        snprintf(s->error, sizeof s->error, "Cannot capture the mouse: %s", SDL_GetError());
        SDL_SetCursor(SDL_GetDefaultCursor());
        return 0;
    }
    if (s->absolute_mouse) {
        int x, y;SDL_GetMouseState(&x, &y);
        mc_video_to_screen(s->video, x, y, &s->absolute_x, &s->absolute_y);
        absolute_move(s);
    }
    if (remote && s->m && s->absolute_mouse != 1) mc_machine_mouse(s->m, 0, 0, s->buttons != 0);
#ifndef _WIN32
    /* With the mouse the keyboard is the Macintosh's too: the desktop's own
     * shortcuts (the Super key opens the configuration window) stay out. */
    SDL_SetWindowKeyboardGrab(mc_video_window(s->video), on ? SDL_TRUE : SDL_FALSE);
#endif
    SDL_SetWindowTitle(mc_video_window(s->video), s->config_open ? TITLE_CONFIG : on ? TITLE_CAPTURED : TITLE_FREE);
    mouse_trace_state(s, on ? "CAPTURE_ON" : "CAPTURE_OFF", 0, 0, (unsigned)remote | (visible_control ? 0x100u : 0u) | (s->absolute_mouse ? 0x200u : 0u));
    return 1;
}
static void notice(shell *s, const char *text) {
    if (s->hidden) { fprintf(stderr, "%s\n", text); return; }
    capture(s, 0);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "Livadix", text, mc_video_window(s->video));
}
static int queue_key(shell *s, uint8_t code, int down) {
    if (((s->write + 1) & 1023) == s->read) { snprintf(s->error, sizeof s->error, "Host key queue overflow"); return 0; }
    s->pending[s->write] = (uint8_t)(code | (down ? 0 : 0x80));
    s->write = (s->write + 1) & 1023;
    return 1;
}
static const char *base_name(const char *path) {
    const char *name = path;
    for (const char *p = path; *p; ++p) if (*p == '/' || *p == '\\') name = p + 1;
    return name;
}
/* SDL's UTF-8 path as the core opens files: by their ANSI names. */
static int ansi_path(const char *utf8, char *path, size_t capacity) {
#ifdef _WIN32
    wchar_t wide[1024];
    return MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wide, 1024) &&
           WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, wide, -1, path, (int)capacity, NULL, NULL);
#else
    /* Linux opens files by their UTF-8 bytes. */
    if (strlen(utf8) >= capacity) return 0;
    memcpy(path, utf8, strlen(utf8) + 1);
    return 1;
#endif
}

/* A dropped file: a DiskCopy 4.2 image or a raw 400, 720, 800 or 1440 KiB image
 * goes into the first empty floppy drive, writable when the file allows it;
 * any other whole number of 2048-byte sectors into an empty CD-ROM drive,
 * read-only. An image already in a drive is reinserted if it was ejected. */
static void drop(shell *s, const char *utf8) {
    mc_machine *m = s->m;
    char path[1024];
    if (!ansi_path(utf8, path, sizeof path)) { notice(s, "Cannot open this file: its name is not supported."); return; }
    /* A .bin with its cue sheet beside it goes in as the cue sheet (its audio tracks too). */
    char cue[1024];
    const char *sheet = mc_disk_cue_sheet(path, cue, sizeof cue);
    if (sheet != path) snprintf(path, sizeof path, "%s", sheet);
    if (s->config_open) { cfg_app_drop(s->app, path); return; }
    if (!m) return;
    static const mc_media_kind kinds[] = {MC_MEDIA_DISK, MC_MEDIA_FLOPPY, MC_MEDIA_CDROM};
    for (unsigned k = 0; k < 3; ++k)
        for (unsigned i = 0; i < mc_machine_drives(m, kinds[k]); ++i) {
            int ejected = 0;
            mc_disk *in = mc_machine_media(m, kinds[k], i, &ejected);
            if (!in || !mc_disk_same_file(in, path)) continue;
            if (kinds[k] == MC_MEDIA_DISK) notice(s, "This image is one of the hard disks.");
            else if (!ejected) notice(s, "This image is already in a drive.");
            else if (mc_machine_insert(m, kinds[k], i, in, NULL, 0)) mc_osd_show(&s->osd, SDL_GetTicks(), "%s reinserted", base_name(path));
            return;
        }
    char error[256] = "";
    mc_disk *disk = mc_disk_open(path, 0, 512, error, sizeof error);
    if (disk && mc_disk_floppy(disk)) {
        mc_disk_close(disk);
        disk = mc_disk_open(path, 1, 512, error, sizeof error);
        if (!disk) disk = mc_disk_open(path, 0, 512, error, sizeof error);
        if (!disk) { notice(s, error); return; }
        for (unsigned i = 0; i < mc_machine_drives(m, MC_MEDIA_FLOPPY); ++i) {
            int ejected = 0;
            mc_disk *in = mc_machine_media(m, MC_MEDIA_FLOPPY, i, &ejected);
            if ((!in || ejected) && mc_machine_insert(m, MC_MEDIA_FLOPPY, i, disk, NULL, 0)) {
                mc_osd_show(&s->osd, SDL_GetTicks(), "Floppy %u: %s", i + 1, base_name(path));
                return;
            }
        }
        mc_disk_close(disk);
        notice(s, mc_machine_drives(m, MC_MEDIA_FLOPPY) ? "No floppy drive is empty." : "This Macintosh has no floppy drive.");
        return;
    }
    mc_disk_close(disk);
    if (!strncmp(error, "DiskCopy", 8)) { notice(s, error); return; }
    disk = mc_disk_open(path, 0, 2048, error, sizeof error);
    if (!disk) {
        notice(s, "Not a disk image Livadix can insert: a floppy image is 400, 720, 800 or 1440 KiB or DiskCopy 4.2, "
                  "a CD image a whole number of 2048-byte sectors.");
        return;
    }
    int ejected = 0;
    mc_disk *in = mc_machine_media(m, MC_MEDIA_CDROM, 0, &ejected);
    if (in && !ejected) { mc_disk_close(disk); notice(s, "The CD-ROM drive is not empty."); return; }
    if (!mc_machine_insert(m, MC_MEDIA_CDROM, 0, disk, error, sizeof error)) { mc_disk_close(disk); notice(s, error); return; }
    mc_osd_show(&s->osd, SDL_GetTicks(), "CD-ROM: %s", base_name(path));
}

/* --- the presenter and the machine's host ends ------------------------------------------------- */

static void apply_look(shell *s) {
    static const int filters[] = {[MC_CONFIG_FILTER_NEAREST] = MC_FILTER_NEAREST, [MC_CONFIG_FILTER_LINEAR] = MC_FILTER_LINEAR,
                                  [MC_CONFIG_FILTER_SHARP] = MC_FILTER_SHARP};
    static const int looks[] = {[MC_LOOK_PLAIN] = MC_VIDEO_LOOK_PLAIN, [MC_LOOK_CRT] = MC_VIDEO_LOOK_CRT, [MC_LOOK_LCD] = MC_VIDEO_LOOK_LCD};
    static const int monitors[] = {[MC_MONITOR_COLOR] = MC_VIDEO_PHOSPHOR_COLOR, [MC_MONITOR_MONO] = MC_VIDEO_PHOSPHOR_WHITE,
                                    [MC_MONITOR_LCD_GRAY] = MC_VIDEO_PHOSPHOR_GRAY};
    mc_video_set_filter(s->video, filters[s->live.filter]);
    /* integer: the largest whole multiple that fits the window. */
    mc_video_set_integer_scale(s->video, s->live.scale == MC_CONFIG_SCALE_INTEGER ? 1 << 16 : 0);
    mc_video_set_vsync(s->video, s->live.vsync && !s->hidden);
    if (!mc_video_set_look(s->video, looks[s->live.look], monitors[s->live.monitor]))
        fputs("video.look and video.monitor need OpenGL 3.3, which is unavailable: the picture is shown plain\n", stderr);
    mc_video_set_geometry(s->video, (int)s->width, (int)s->height);
}

static void attach_input(shell *s) {
    mc_machine_set_audio_source(s->m, NULL);
    mc_host_capture_close(&s->microphone);
    if (!s->hidden && s->live.sound) {
        const mc_audio_source source = mc_host_capture_callbacks(&s->microphone);
        mc_machine_set_audio_source(s->m, &source);
    }
}

/* The machine's sound, serial ports and network; 0 with the reason. */
static int attach(shell *s, char *error, size_t capacity) {
    mc_machine *m = s->m;
    memset(&s->audio, 0, sizeof s->audio);
    if (!s->options->wav && mc_machine_audio_rate(m) && (s->live.sound || s->live.floppy_sound) &&
        (SDL_InitSubSystem(SDL_INIT_AUDIO) || !mc_host_audio_open(&s->audio)))
        fprintf(stderr, "Sound output unavailable: %s\n", SDL_GetError());
    if (!mc_host_serial_open(&s->serial, s->live.ports, error, capacity)) { mc_host_audio_close(&s->audio); return 0; }
    s->serial_host = mc_host_serial_callbacks(&s->serial);
    mc_machine_set_serial(m, &s->serial_host);
    /* The Ethernet's network (network.mode): the NAT on its own thread. */
    error[0] = 0;
    s->net = mc_net_open_config(&s->live, error, capacity);
    if (!s->net && error[0]) {
        mc_machine_set_serial(m, NULL);
        mc_host_serial_close(&s->serial);
        mc_host_audio_close(&s->audio);
        return 0;
    }
    mc_net_attach(s->net, m);
    attach_input(s);
    return 1;
}
/* The machine stopped: its host ends closed and, when the session built
 * it, the machine itself with its disks (flushed and closed). */
static void detach(shell *s) {
    if (!s->m) return;
    mc_machine_set_audio_source(s->m, NULL);
    mc_host_capture_close(&s->microphone);
    mc_machine_set_serial(s->m, NULL);
    mc_host_serial_close(&s->serial);
    mc_machine_set_net(s->m, NULL);
    mc_net_close(s->net);
    s->net = NULL;
    mc_host_audio_close(&s->audio);
    memset(&s->audio, 0, sizeof s->audio);
    if (s->owns && !mc_machine_close(s->m,s->error,sizeof s->error)) {
        s->save_failed=1;
        fprintf(stderr,"%s\n",s->error);
        if(s->app)cfg_app_set_problem(s->app,s->error);
        if(!s->hidden)SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR,"NVRAM could not be saved",s->error,NULL);
    }
    s->m = NULL;
    s->closing = 0;
    s->read = s->write = 0;
    memset(s->pressed, 0, sizeof s->pressed);
    s->buttons = 0;
}
/* The screen's size: the machine's frame, else the configuration's. */
static void screen_size(shell *s) {
    mc_framebuffer fb;
    memset(&fb, 0, sizeof fb);
    s->width = s->live.width ? s->live.width : 640;
    s->height = s->live.height ? s->live.height : 480;
    if (s->m && mc_machine_framebuffer(s->m, &fb) && fb.width && fb.height) { s->width = fb.width; s->height = fb.height; }
}

/* --- no Macintosh running: the logo ------------------------------------------------------------- */

/* The logo screen at w x h: the configuration window's gray with the
 * Livadix logo centred, resampled bilinearly (premultiplied). Kept until the
 * size changes. NULL without memory. */
static const uint32_t *logo_screen(shell *s, int w, int h) {
    if (w <= 0 || h <= 0) return NULL;
    if (s->logo && s->logo_w == w && s->logo_h == h) return s->logo;
    uint32_t *px = realloc(s->logo, (size_t)w * h * sizeof *px);
    if (!px) return NULL;
    s->logo = px;
    s->logo_w = w;
    s->logo_h = h;
    const uint32_t back = 0xDDDDDD;
    for (size_t i = 0; i < (size_t)w * h; ++i) px[i] = back;
    int lw = w * 2 / 5;
    if (lw > h * 5 / 8) lw = h * 5 / 8;
    const int lh = lw * MC_LOGO_HEIGHT / MC_LOGO_WIDTH;
    if (lw < 2 || lh < 2) return px;
    const int x0 = (w - lw) / 2, y0 = (h - lh) / 2;
    for (int y = 0; y < lh; ++y) {
        const double fy = (y + 0.5) * MC_LOGO_HEIGHT / lh - 0.5;
        const int sy = fy < 0 ? 0 : (int)fy, sy1 = sy + 1 < MC_LOGO_HEIGHT ? sy + 1 : sy;
        const double ty = fy < 0 ? 0 : fy - sy;
        for (int x = 0; x < lw; ++x) {
            const double fx = (x + 0.5) * MC_LOGO_WIDTH / lw - 0.5;
            const int sx = fx < 0 ? 0 : (int)fx, sx1 = sx + 1 < MC_LOGO_WIDTH ? sx + 1 : sx;
            const double tx = fx < 0 ? 0 : fx - sx;
            const uint32_t q[4] = {MC_LOGO_ARGB[(size_t)sy * MC_LOGO_WIDTH + sx], MC_LOGO_ARGB[(size_t)sy * MC_LOGO_WIDTH + sx1],
                                   MC_LOGO_ARGB[(size_t)sy1 * MC_LOGO_WIDTH + sx], MC_LOGO_ARGB[(size_t)sy1 * MC_LOGO_WIDTH + sx1]};
            const double wt[4] = {(1 - tx) * (1 - ty), tx * (1 - ty), (1 - tx) * ty, tx * ty};
            double a = 0, c[3] = {0, 0, 0};
            for (int k = 0; k < 4; ++k) {
                const double qa = (q[k] >> 24) / 255.0;
                a += wt[k] * qa;
                for (int n = 0; n < 3; ++n) c[n] += wt[k] * qa * ((q[k] >> (8 * n)) & 0xFF);
            }
            uint32_t out = 0;
            for (int n = 0; n < 3; ++n) {
                const double v = c[n] + (1 - a) * ((back >> (8 * n)) & 0xFF);
                out |= (uint32_t)(v + 0.5 > 255 ? 255 : v + 0.5) << (8 * n);
            }
            px[(size_t)(y0 + y) * w + x0 + x] = out;
        }
    }
    return px;
}
/* The logo screen filling the window. */
static void logo_present(shell *s) {
    int w = (int)s->width, h = (int)s->height;
    if (!s->hidden) SDL_GetWindowSizeInPixels(mc_video_window(s->video), &w, &h);
    const uint32_t *px = logo_screen(s, w, h);
    if (!px) return;
    mc_video_set_look(s->video, MC_VIDEO_LOOK_PLAIN, MC_VIDEO_PHOSPHOR_COLOR);
    mc_video_set_integer_scale(s->video, 0);
    mc_video_set_filter(s->video, MC_FILTER_NEAREST);
    mc_video_set_geometry(s->video, w, h);
    mc_video_present(s->video, px, w, h);
    if (!s->hidden && !s->config_open) SDL_SetWindowTitle(mc_video_window(s->video), TITLE_STOPPED);
    s->logo_shown = 1;
}

/* --- the configuration window ------------------------------------------------------------------- */

static void config_start(void *ctx, const char *profile, const mc_config *config) {
    shell *s = ctx;
    s->restart = 1;
    s->restart_config = *config;
    s->restart_has_profile = profile != NULL;
    snprintf(s->restart_profile, sizeof s->restart_profile, "%s", profile ? profile : "");
}
static void config_resume(void *ctx) { ((shell *)ctx)->resume = 1; }
static void config_quit(void *ctx) { ((shell *)ctx)->quit = ((shell *)ctx)->asked = 1; }
static void config_stop(void *ctx) { ((shell *)ctx)->stop = 1; }
static int config_ready(shell *s) {
    if (s->app) return 1;
    if (!s->session) return 0;
    s->app = calloc(1, sizeof *s->app);
    const cfg_host host = {s, config_start, config_resume, config_quit, config_stop, config_resume};
    if (!s->app || !cfg_app_init(s->app, s->session->yaml, s->session->program_folder, s->session->roms, &host)) {
        free(s->app);
        s->app = NULL;
        return 0;
    }
    cfg_host_serial_ports(s->app);
    return 1;
}
/* The canvas the configuration window is drawn on: the window's size, in
 * whole multiples that keep it at least the layout's size; never smaller
 * than the layout (820 x 600), which a smaller window or display then
 * shows scaled down. */
static void config_canvas(shell *s, int *w, int *h, int *scale) {
    *scale = 1;
    if (s->hidden) {
        *w = s->session && s->session->width > 0 ? s->session->width : CONFIG_W;
        *h = s->session && s->session->height > 0 ? s->session->height : CONFIG_H;
        return;
    }
    int ww = 0, wh = 0;
    SDL_GetWindowSize(mc_video_window(s->video), &ww, &wh);
    const int k = ww / CONFIG_W < wh / CONFIG_H ? ww / CONFIG_W : wh / CONFIG_H;
    *scale = k > 1 ? k : 1;
    *w = ww / *scale;
    *h = wh / *scale;
    if (*w < CONFIG_W) *w = CONFIG_W;
    if (*h < CONFIG_H) *h = CONFIG_H;
}
static void config_frame(shell *s, const ui_input *in);
static void config_open(shell *s) {
    if (s->config_open || !config_ready(s)) return;
    s->config_open = 1;
    cfg_app_set_running(s->app, s->m ? (s->has_profile ? s->profile : NULL) : NULL);
    if (s->m && s->has_profile) cfg_app_select(s->app, s->profile);
    /* What changes the window makes are measured from here: settings the
     * window corrects on opening wait for the next start. */
    if (!s->m || !cfg_app_profile(s->app, s->has_profile ? s->profile : NULL, &s->opened)) s->opened = s->started;
    s->app->host.resume = s->m ? config_resume : NULL;
    s->media_mark = s->app->media_chosen;
    if (s->m && !s->built_set) {
        s->built = s->opened;
        s->built.sound = s->started.sound;         /* sound on again needs a machine built with it */
        s->built_set = 1;
    }
    s->app->running_from = s->m ? &s->built : NULL;
    capture(s, 0);
    /* The window large enough for the configuration's layout. */
    if (!s->hidden && !mc_video_fullscreen(s->video)) {
        SDL_Window *w = mc_video_window(s->video);
        SDL_GetWindowSize(w, &s->window_w, &s->window_h);
        if (s->window_w < CONFIG_W || s->window_h < CONFIG_H) {
            int want_w = s->window_w > CONFIG_W ? s->window_w : CONFIG_W, want_h = s->window_h > CONFIG_H ? s->window_h : CONFIG_H;
            /* No larger than the display's work area, frame included: on a
             * smaller display the layout is shown scaled down (config_canvas). */
            SDL_Rect area;
            int top = 0, left = 0, bottom = 0, right = 0;
            const int d = SDL_GetWindowDisplayIndex(w);
            SDL_GetWindowBordersSize(w, &top, &left, &bottom, &right);
            if (d >= 0 && SDL_GetDisplayUsableBounds(d, &area) == 0) {
                if (want_w > area.w - left - right) want_w = area.w - left - right;
                if (want_h > area.h - top - bottom) want_h = area.h - top - bottom;
            }
            if (want_w > s->window_w || want_h > s->window_h) {
                SDL_SetWindowSize(w, want_w > s->window_w ? want_w : s->window_w, want_h > s->window_h ? want_h : s->window_h);
                int x = 0, y = 0;
                SDL_GetWindowPosition(w, &x, &y);
                if (d >= 0 && SDL_GetDisplayUsableBounds(d, &area) == 0) {
                    /* Kept on the work area, title bar included. */
                    if (x + want_w + right > area.x + area.w) x = area.x + area.w - want_w - right;
                    if (y + want_h + bottom > area.y + area.h) y = area.y + area.h - want_h - bottom;
                    if (x - left < area.x) x = area.x + left;
                    if (y - top < area.y) y = area.y + top;
                    SDL_SetWindowPosition(w, x, y);
                }
                s->window_resized = 1;
            }
        }
    }
    SDL_StartTextInput();
    printf("Configuration: opened%s\n", s->m ? ", the Macintosh paused" : "");
    fflush(stdout);
    ui_input idle;
    memset(&idle, 0, sizeof idle);
    config_frame(s, &idle);
}
static void config_close(shell *s) {
    if (!s->config_open) return;
    s->config_open = 0;
    if (s->window_resized && !mc_video_fullscreen(s->video)) SDL_SetWindowSize(mc_video_window(s->video), s->window_w, s->window_h);
    s->window_resized = 0;
    mc_video_set_overlay(s->video, NULL, 0, 0);
    SDL_StopTextInput();
    if (!s->hidden) SDL_SetWindowTitle(mc_video_window(s->video), TITLE_FREE);
    screen_size(s);
    apply_look(s);
    s->logo_shown = 0;
    /* Back to the running Macintosh: the mouse is its again, as after a
     * click on its screen (stopped() lets it go when it is off). */
    if (s->m && !s->hidden && (SDL_GetWindowFlags(mc_video_window(s->video)) & SDL_WINDOW_INPUT_FOCUS)) capture(s, 1);
}

/* Stop: the Macintosh is off (its disks closed, its NVRAM saved), and the
 * window shows the logo until a profile starts or Livadix quits. */
static void stopped(shell *s) {
    detach(s);
    s->stopping = 0;
    s->built_set = 0;
    if (s->app) {
        cfg_app_set_running(s->app, NULL);
        s->app->host.resume = NULL;
        s->app->running_from = NULL;
    }
    config_close(s);
    capture(s, 0);
    screen_size(s);
    s->logo_shown = 0;
    printf("Stopped: the Macintosh is off\n");
    fflush(stdout);
}

/* The shutdown of Stop or Restart ended: the Macintosh is off, and with
 * Restart it starts again. */
static void shutdown_done(shell *s) {
    stopped(s);
    if (s->restart_queued) { s->restart_queued = 0; s->restart = 1; }
}

/* Starts a configuration: the running machine stops and its disks close
 * first. 0 when it cannot start: the configuration window shows why. */
static int start_machine(shell *s, const mc_config *config, const char *profile) {
    detach(s);
    s->built_set = 0;
    s->live = s->started = *config;
    s->has_profile = profile != NULL;
    snprintf(s->profile, sizeof s->profile, "%s", profile ? profile : "");
    char problem[1024] = "";
    mc_machine *m = s->session->build(s->session->ctx, config, problem, sizeof problem);
    if (m) {
        s->m = m;
        s->owns = 1;
        s->logo_shown = 0;
        if (attach(s, problem, sizeof problem)) {
            screen_size(s);
            if (!s->hidden && !mc_video_fullscreen(s->video)) {
                SDL_SetWindowSize(mc_video_window(s->video), (int)s->width, (int)s->height);
                s->window_w = (int)s->width;
                s->window_h = (int)s->height;
            }
            if (s->live.fullscreen != mc_video_fullscreen(s->video) && !s->hidden) mc_video_set_fullscreen(s->video, s->live.fullscreen);
            printf("Configuration: started %s%s%s\n", profile ? "the profile \"" : "the configuration", profile ? profile : "", profile ? "\"" : "");
            fflush(stdout);
            if (s->app) { cfg_app_set_problem(s->app, NULL); cfg_app_set_running(s->app, profile); }
            config_close(s);
            return 1;
        }
        mc_machine_destroy(m);
        s->m = NULL;
    }
    printf("Configuration: the Macintosh could not start: %s\n", problem);
    fflush(stdout);
    if (config_ready(s)) {
        if (!s->config_open) config_open(s);
        if (profile) cfg_app_select(s->app, profile);
        cfg_app_set_problem(s->app, problem);
        cfg_app_set_running(s->app, NULL);
        s->app->host.resume = NULL;
    }
    return 0;
}

/* The floppy and CD images the configuration window chose, as a person
 * changes the disks of a running Macintosh: when the window chose a drive's
 * disk (even the same one again, after the System ejected it) the disk in
 * the drive comes out and the chosen one goes in, and the System finds it as
 * it would a real one. A CD the System holds (PREVENT MEDIUM REMOVAL) stays,
 * as a real drive ignores its eject button then, and so does the profile's
 * CD: the System ejects it (Put Away). A floppy drive has no button: its
 * disk comes out as with a paper clip. A drive's index counts the fitted
 * floppy drives only. */
static int media_path_same(const char *a, const char *b) {
    for (;; ++a, ++b) {
        const char x = *a == '\\' ? '/' : (char)tolower((unsigned char)*a), y = *b == '\\' ? '/' : (char)tolower((unsigned char)*b);
        if (x != y) return 0;
        if (!x) return 1;
    }
}
/* The disk in a drive (NULL when empty or ejected) is the image named. */
static int media_in(mc_machine *m, mc_media_kind kind, unsigned drive, const char *image) {
    int ejected = 0;
    mc_disk *in = mc_machine_media(m, kind, drive, &ejected);
    return in && !ejected ? image[0] && mc_disk_same_file(in, image) : !image[0];
}
static void media_put(shell *s, mc_media_kind kind, unsigned drive, const char *image, int readonly) {
    char error[256] = "";
    mc_machine_eject(s->m, kind, drive);
    if (!image[0]) return;
    mc_disk *d = kind == MC_MEDIA_CDROM ? mc_disk_open(image, 0, 2048, error, sizeof error) : mc_disk_open(image, !readonly, 512, error, sizeof error);
    if (!d && kind == MC_MEDIA_FLOPPY && !readonly) d = mc_disk_open(image, 0, 512, error, sizeof error);
    if (!d || !mc_machine_insert(s->m, kind, drive, d, error, sizeof error)) {
        mc_disk_close(d);
        notice(s, error);
    }
}
static void apply_media(shell *s, const mc_config *from, const mc_config *to, int chosen) {
    mc_machine *m = s->m;
    unsigned drive = 0;
    for (size_t i = 0; i < to->floppy_count; ++i) {
        if (to->floppy_off[i]) continue;
        const unsigned index = drive++;
        const mc_disk_config *a = &from->floppies[i], *b = &to->floppies[i];
        const int changed = !media_path_same(a->image, b->image) || (b->image[0] && a->readonly != b->readonly);
        if (!changed && (!chosen || media_in(m, MC_MEDIA_FLOPPY, index, b->image))) continue;
        media_put(s, MC_MEDIA_FLOPPY, index, b->image, b->readonly);
    }
    if (media_path_same(from->cdrom, to->cdrom) && (!chosen || media_in(m, MC_MEDIA_CDROM, 0, to->cdrom))) return;
    int ejected = 0;
    mc_disk *in = mc_machine_media(m, MC_MEDIA_CDROM, 0, &ejected);
    if (in && !ejected && mc_disk_locked(in)) {
        if (!media_path_same(from->cdrom, to->cdrom)) cfg_app_put_cdrom(s->app, from->cdrom);
        memcpy(s->live.cdrom, from->cdrom, sizeof s->live.cdrom);
        notice(s, "The Macintosh is using the CD in its drive: eject it there first (drag its icon to the Trash), "
                  "then insert the other one.");
        return;
    }
    media_put(s, MC_MEDIA_CDROM, 0, to->cdrom, 1);
}

/* The configuration window closed: the running profile's changes that apply
 * as the Macintosh runs do; those that need a new start wait for one (the
 * window's Restart). None running: the logo. */
static void config_apply(shell *s) {
    if (!s->m) { config_close(s); return; }
    mc_config edited;
    if (!cfg_app_profile(s->app, s->has_profile ? s->profile : NULL, &edited)) { config_close(s); return; }
    mc_config from = s->opened;
    from.sound = s->started.sound;                 /* sound on again needs a machine built with it */
    const int chosen = s->app->media_chosen != s->media_mark;
    if (mc_profile_same(&from, &edited) && from.sound == s->live.sound && !chosen) {
        printf("Configuration: closed; nothing changed\n");
        fflush(stdout);
        config_close(s);
        return;
    }
    const int waits = mc_profile_needs_restart(&s->built, &edited);
    /* Disks go into the drives this Macintosh was built with. */
    int drives = s->built.model == edited.model && s->built.cdrom_bus == edited.cdrom_bus && s->built.floppy_count == edited.floppy_count;
    for (size_t i = 0; drives && i < edited.floppy_count; ++i) drives = s->built.floppy_off[i] == edited.floppy_off[i];
    if (drives) {
        memcpy(s->live.cdrom, edited.cdrom, sizeof s->live.cdrom);
        memcpy(s->live.floppies, edited.floppies, sizeof s->live.floppies);
        apply_media(s, &from, &edited, chosen);
    }
    if (!s->started.sound) edited.sound = 0;       /* on again: with the new start */
    const int sound = s->live.sound;
    const int output = sound || s->live.floppy_sound;
    s->live.startup_sound = edited.startup_sound;
    s->live.close_action = edited.close_action;
    s->live.close_timeout = edited.close_timeout;
    s->live.refresh = edited.refresh;
    s->live.look = edited.look;
    s->live.monitor = edited.monitor;
    s->live.filter = edited.filter;
    s->live.scale = edited.scale;
    s->live.vsync = edited.vsync;
    s->live.sound = edited.sound;
    s->live.floppy_sound = edited.floppy_sound;
    mc_machine_set_sound(s->m,edited.sound,edited.floppy_sound);
    s->live.command_key = edited.command_key;
    s->live.mouse[0] = edited.mouse[0];
    s->live.mouse[1] = edited.mouse[1];
    if (edited.fullscreen != s->live.fullscreen && !s->hidden) mc_video_set_fullscreen(s->video, edited.fullscreen);
    s->live.fullscreen = edited.fullscreen;
    /* Sound off: the samples go nowhere; on again: to the output. */
    if (output && !(s->live.sound || s->live.floppy_sound)) { mc_host_audio_close(&s->audio); memset(&s->audio, 0, sizeof s->audio); }
    else if (!output && (s->live.sound || s->live.floppy_sound) && !s->options->wav && mc_machine_audio_rate(s->m) &&
             (SDL_InitSubSystem(SDL_INIT_AUDIO) || !mc_host_audio_open(&s->audio)))
        fprintf(stderr, "Sound output unavailable: %s\n", SDL_GetError());
    if (sound != s->live.sound) attach_input(s);
    printf("Configuration: closed; %s\n", waits ? "changes wait for a new start" : "the changes apply as the Macintosh runs");
    fflush(stdout);
    config_close(s);
}

/* One frame of the configuration window, drawn and shown. */
/* The window drawn and made the overlay, not shown yet (config_frame shows it). */
static void config_draw(shell *s, const ui_input *in) {
    int w, h, scale;
    config_canvas(s, &w, &h, &scale);
    ui_canvas *c = &s->app->ui.canvas;
    if (!c->px || c->w != w || c->h != h) {
        uint32_t *px = realloc(c->px, (size_t)w * h * sizeof *px);
        if (!px) return;
        c->px = px;
        c->w = w;
        c->h = h;
        ui_clear_clip(c);
    }
    /* The window, at least the layout's size, drawn over the screen as it
     * is: the running Mac's, or the logo screen. What it leaves untouched is
     * transparent in the overlay. */
    const uint32_t clear = 0x00FE01FDu;
    for (size_t i = 0; i < (size_t)w * h; ++i) c->px[i] = clear;
    const int ww = w > 1100 ? w - (w - 1100) / 2 : w, wh = h > 760 ? h - (h - 760) / 2 : h;
    cfg_app_frame(s->app, in, (ui_rect){(w - ww) / 2, (h - wh) / 2, ww, wh});
    uint32_t *over = realloc(s->overlay, (size_t)w * h * sizeof *over);
    if (!over) return;
    s->overlay = over;
    for (size_t i = 0; i < (size_t)w * h; ++i) over[i] = c->px[i] == clear ? 0 : c->px[i] | 0xFF000000u;
    mc_video_set_overlay(s->video, over, w, h);
}
/* A frame of the window, shown. */
static void config_frame(shell *s, const ui_input *in) {
    config_draw(s, in);
    if (s->m) mc_video_present(s->video, s->pixels, (int)s->width, (int)s->height);
    else logo_present(s);
}
/* The mouse over the configuration window's canvas. */
static void config_mouse(shell *s, ui_input *in) {
    int mx = 0, my = 0, ww = 0, wh = 0;
    SDL_GetMouseState(&mx, &my);
    SDL_GetWindowSize(mc_video_window(s->video), &ww, &wh);
    const ui_canvas *c = &s->app->ui.canvas;
    const SDL_Rect o = mc_video_overlay_rect(c->w, c->h, ww, wh);
    in->x = o.w > 0 ? (int)((int64_t)(mx - o.x) * c->w / o.w) : mx;
    in->y = o.h > 0 ? (int)((int64_t)(my - o.y) * c->h / o.h) : my;
}
/* The window's input to the configuration window, from one event. */
static void config_event_input(shell *s, const SDL_Event *e, ui_input *in) {
    /* The modifiers held with every event, clicks too (Ctrl- and Shift-click). */
    const SDL_Keymod held = SDL_GetModState();
    in->mods = (held & KMOD_SHIFT ? UI_MOD_SHIFT : 0) | (held & KMOD_CTRL ? UI_MOD_CTRL : 0) | (held & KMOD_ALT ? UI_MOD_ALT : 0);
    switch (e->type) {
    case SDL_MOUSEBUTTONDOWN:
        if (e->button.button == SDL_BUTTON_LEFT) {
            in->pressed = 1;
            in->double_click = e->button.clicks > 1 || SDL_GetTicks() - s->last_click < 400;
            s->last_click = SDL_GetTicks();
        }
        break;
    case SDL_MOUSEBUTTONUP: if (e->button.button == SDL_BUTTON_LEFT) in->released = 1; break;
    case SDL_MOUSEWHEEL: in->wheel = e->wheel.y; break;
    case SDL_KEYDOWN: {
        switch (e->key.keysym.sym) {
        case SDLK_TAB: in->key = UI_KEY_TAB; break;
        case SDLK_RETURN: case SDLK_KP_ENTER: in->key = UI_KEY_ENTER; break;
        case SDLK_ESCAPE: in->key = UI_KEY_ESCAPE; break;
        case SDLK_SPACE: in->key = UI_KEY_SPACE; break;
        case SDLK_UP: in->key = UI_KEY_UP; break;
        case SDLK_DOWN: in->key = UI_KEY_DOWN; break;
        case SDLK_LEFT: in->key = UI_KEY_LEFT; break;
        case SDLK_RIGHT: in->key = UI_KEY_RIGHT; break;
        case SDLK_HOME: in->key = UI_KEY_HOME; break;
        case SDLK_END: in->key = UI_KEY_END; break;
        case SDLK_BACKSPACE: in->key = UI_KEY_BACKSPACE; break;
        case SDLK_DELETE: in->key = UI_KEY_DELETE; break;
        case SDLK_PAGEUP: in->key = UI_KEY_PAGE_UP; break;
        case SDLK_PAGEDOWN: in->key = UI_KEY_PAGE_DOWN; break;
        default:
            if (e->key.keysym.sym >= 'a' && e->key.keysym.sym <= 'z' && (in->mods & UI_MOD_CTRL)) {
                in->key = UI_KEY_CHAR;
                in->ch = (int)e->key.keysym.sym;
            }
            break;
        }
        break;
    }
    case SDL_TEXTINPUT: snprintf(in->text, sizeof in->text, "%s", e->text.text); break;
    default: break;
    }
}

/* --- checks: the script ------------------------------------------------------------------------ */

static int pad_console(const shell *s);
static int pad_event(shell *s, const SDL_Event *e);

static void script_frame(shell *s, const ui_input *in) { if (s->config_open) config_frame(s, in); }
static void script_idle(shell *s, int x, int y) {
    ui_input in;
    memset(&in, 0, sizeof in);
    in.x = x; in.y = y;
    script_frame(s, &in);
}
static void script_click(shell *s, int x, int y, int twice) {
    ui_input in;
    memset(&in, 0, sizeof in);
    in.x = x; in.y = y;
    script_frame(s, &in);
    in.pressed = 1;
    in.double_click = twice;
    script_frame(s, &in);
    in.pressed = 0;
    in.released = 1;
    script_frame(s, &in);
    script_idle(s, x, y);
}
static int script_press(shell *s, const char *label) {
    ui *u = &s->app->ui;
    /* An item of an open menu scrolled out of sight is scrolled to first. */
    int shown = 0;
    for (int i = 0; i < u->hit_count && !shown; ++i) shown = !strcmp(u->hits[i].label, label);
    if (!shown && ui_menu_reveal(u, label)) script_idle(s, u->in.x, u->in.y);
    for (int i = 0; i < u->hit_count; ++i)
        if (!strcmp(u->hits[i].label, label)) {
            const ui_rect r = u->hits[i].r;
            script_click(s, r.x + (r.w < 24 ? r.w / 2 : 8), r.y + r.h / 2, 0);
            return 1;
        }
    return 0;
}
/* Runs the script's commands until one waits for frames, ends the
 * configuration window, or the script ends. 0 on a failed command. */
static int script_step(shell *s) {
    cfg_script *sc = &s->script;
    char line[2048];
    while (!sc->done && !s->script_wait && !s->restart && !s->resume && !s->quit && !s->stop) {
        const long at = ftell(sc->f);
        if (!fgets(line, sizeof line, sc->f)) { sc->done = 1; break; }
        sc->line++;
        line[strcspn(line, "\r\n")] = 0;
        char *w[8];
        const int n = cfg_script_words(line, w, 8);
        if (!n) continue;
        int failed = 0;
        const int needs_window = strcmp(w[0], "frames") && strcmp(w[0], "open") && strcmp(w[0], "expect-open") &&
                                 strcmp(w[0], "expect-running") && strcmp(w[0], "expect-config") && strcmp(w[0], "mouse-to") &&
                                 strcmp(w[0], "expect-mouse") && strcmp(w[0], "wait-system") && strcmp(w[0], "pad") && strcmp(w[0], "button") &&
                                 strcmp(w[0], "absolute-mouse") && strcmp(w[0], "screenshot");
        if (needs_window && !s->config_open) {
            fprintf(stderr, "script line %d: the configuration window is not open for \"%s\"\n", sc->line, line);
            return 0;
        }
        cfg_app *a = s->app;
        if (!strcmp(w[0], "frames") && n == 2) s->script_wait = strtoull(w[1], NULL, 10);
        /* Classic Mac OS cursor globals: where Remote Desktop places the
         * pointer (absolute_move), and where the System's then is. */
        else if (!strcmp(w[0], "wait-system") && n == 1) {   /* again next frame until a System shows its desktop */
            if (!s->m || !mc_shutdown_desktop_running(s->m)) { fseek(sc->f, at, SEEK_SET); sc->line--; s->script_wait = 1; }
        }
        else if (!strcmp(w[0], "mouse-to") && n == 3) failed = !s->m || !mc_machine_mouse_to(s->m, atoi(w[1]), atoi(w[2]));
        else if (!strcmp(w[0], "absolute-mouse") && n == 4) {   /* the pointer and button as a Remote Desktop session sends them */
            s->absolute_mouse = absolute_mode(s);
            s->absolute_x = atoi(w[1]);
            s->absolute_y = atoi(w[2]);
            s->buttons = atoi(w[3]) != 0;
            failed = !s->absolute_mouse || !absolute_move(s);
            if (!failed && s->absolute_mouse == 2) click_push(s);
        }
        else if (!strcmp(w[0], "button") && n == 2) failed = !s->m || !mc_machine_mouse(s->m, 0, 0, atoi(w[1]) != 0);   /* the Macintosh's mouse button */
        else if (!strcmp(w[0], "expect-mouse") && n == 3) {
            uint32_t v = 0, h = 0;
            failed = !s->m || !mc_machine_peek(s->m, 0x830, 2, &v) || !mc_machine_peek(s->m, 0x832, 2, &h) ||
                     (int)h != atoi(w[1]) || (int)v != atoi(w[2]);
            if (failed) fprintf(stderr, "script line %d: the cursor is at %u,%u\n", sc->line, (unsigned)h, (unsigned)v);
        }
        else if (!strcmp(w[0], "open") && n == 1) config_open(s);
        else if (!strcmp(w[0], "click") && n == 3) script_click(s, atoi(w[1]), atoi(w[2]), 0);
        else if (!strcmp(w[0], "doubleclick") && n == 3) script_click(s, atoi(w[1]), atoi(w[2]), 1);
        else if (!strcmp(w[0], "pad") && n == 3) {             /* a game controller's button (SDL's name), 1 down or 0 up */
            SDL_Event e;
            memset(&e, 0, sizeof e);
            const SDL_GameControllerButton button = SDL_GameControllerGetButtonFromString(w[1]);
            e.type = atoi(w[2]) ? SDL_CONTROLLERBUTTONDOWN : SDL_CONTROLLERBUTTONUP;
            e.cbutton.button = (Uint8)button;
            failed = button == SDL_CONTROLLER_BUTTON_INVALID || !pad_console(s);
            if (!failed) pad_event(s, &e);
        }
        else if (!strcmp(w[0], "press") && n == 2) failed = !script_press(s, w[1]);
        else if (!strcmp(w[0], "choose") && n == 3)
            failed = !script_press(s, w[1]) || !a->ui.menu.id || !script_press(s, w[2]);
        else if (!strcmp(w[0], "key") && n == 2) {
            ui_input in;
            memset(&in, 0, sizeof in);
            int mods = 0, k = cfg_script_key(w[1], &mods);
            /* The Windows key: the window closes, back to the Macintosh. */
            if (!strcmp(w[1], "win")) {
                if (!s->m) failed = 1;
                else if (s->config_open) { cfg_app_close(a); script_idle(s, 0, 0); }
            }
            else if (!k) failed = 1;
            else {
                if (k < 0) { in.key = UI_KEY_CHAR; in.ch = -k; } else in.key = k;
                in.mods = mods;
                script_frame(s, &in);
                script_idle(s, 0, 0);
            }
        }
        else if (!strcmp(w[0], "type") && n == 2) {
            ui_input in;
            memset(&in, 0, sizeof in);
            snprintf(in.text, sizeof in.text, "%s", w[1]);
            script_frame(s, &in);
            script_idle(s, 0, 0);
        }
        else if (!strcmp(w[0], "pick") && n == 2) {
            if (!a->pending_pick) failed = 1;
            else { cfg_app_pick_result(a, w[1]); script_idle(s, 0, 0); }
        }
        else if (!strcmp(w[0], "drop") && n == 2) { cfg_app_drop(a, w[1]); script_idle(s, 0, 0); }
        else if (!strcmp(w[0], "select") && n == 2) { failed = !cfg_app_select(a, w[1]); script_idle(s, 0, 0); }
        else if (!strcmp(w[0], "screenshot") && n == 2) {
            if (s->config_open) {script_idle(s, 0, 0);failed = !cfg_host_save_bmp(&a->ui.canvas, w[1]);}
            else { mc_video_request_shot(s->video, w[1], true); if (!s->m) s->logo_shown = 0; }
        }
        else if (!strcmp(w[0], "close") && n == 1) { cfg_app_quit(a); script_idle(s, 0, 0); }
        else if (!strcmp(w[0], "expect") && n == 2) {
            failed = !strstr(a->status, w[1]);
            if (failed) fprintf(stderr, "script line %d: the status line \"%s\" lacks \"%s\"\n", sc->line, a->status, w[1]);
        }
        else if (!strcmp(w[0], "expect-label") && n == 2) {
            failed = 1;
            for (int i = 0; i < a->ui.hit_count; ++i) if (!strcmp(a->ui.hits[i].label, w[1])) failed = 0;
        }
        else if (!strcmp(w[0], "expect-open") && n == 2) failed = s->config_open != !strcmp(w[1], "yes");
        else if (!strcmp(w[0], "expect-running") && n == 2)
            failed = !s->m || (strcmp(w[1], "-") ? !s->has_profile || strcmp(s->profile, w[1]) : s->has_profile);
        else if (!strcmp(w[0], "expect-config") && n == 3) {
            /* A value in force: look, filter, depth, ram, model, sound. */
            char got[64] = "";
            if (!strcmp(w[1], "look")) snprintf(got, sizeof got, "%s", mc_config_look_name(s->live.look));
            else if (!strcmp(w[1], "filter")) snprintf(got, sizeof got, "%s", mc_config_filter_name(s->live.filter));
            else if (!strcmp(w[1], "depth")) snprintf(got, sizeof got, "%u", (unsigned)s->live.depth);
            else if (!strcmp(w[1], "resolution")) snprintf(got, sizeof got, "%ux%u", (unsigned)s->live.width, (unsigned)s->live.height);
            else if (!strcmp(w[1], "ram")) mc_config_ram_text(mc_config_ram_kb(&s->live), got, sizeof got);
            else if (!strcmp(w[1], "sound")) snprintf(got, sizeof got, "%s", s->live.sound ? "on" : "off");
            else if (!strcmp(w[1], "floppy")) snprintf(got, sizeof got, "%s", s->live.floppy_sound ? "on" : "off");
            failed = strcmp(got, w[2]) != 0;
            if (failed) fprintf(stderr, "script line %d: %s is %s, not %s\n", sc->line, w[1], got, w[2]);
        }
        else failed = 1;
        if (failed) { fprintf(stderr, "script line %d: cannot do \"%s\"\n", sc->line, line); return 0; }
    }
    return 1;
}

/* --- the window's events ------------------------------------------------------------------------ */

/* A host game controller on the Pippin (docs/NOTES.md, "The
 * Pippin"): the buttons become the host keys the Applejack is driven by (the
 * directional pad the arrow keys, A, B, X and Y the green, red, blue and
 * yellow buttons' F1 to F4, Back, Guide and Start the square, circle and
 * diamond buttons' F5 to F7, the right shoulder Right Fire's F8), the left
 * shoulder the mouse button (Left Fire); the left stick moves the trackball
 * (pad_frame). Other models ignore the controller. */
#define PAD_FIRE 0x100u
static int pad_console(const shell *s) { return s->m && mc_model_entry(s->live.model)->console; }
static int pad_event(shell *s, const SDL_Event *e) {
    static const struct { SDL_GameControllerButton button; uint8_t key; } map[] = {
        {SDL_CONTROLLER_BUTTON_DPAD_UP, 0x3E}, {SDL_CONTROLLER_BUTTON_DPAD_DOWN, 0x3D},
        {SDL_CONTROLLER_BUTTON_DPAD_LEFT, 0x3B}, {SDL_CONTROLLER_BUTTON_DPAD_RIGHT, 0x3C},
        {SDL_CONTROLLER_BUTTON_A, 0x7A}, {SDL_CONTROLLER_BUTTON_B, 0x78},
        {SDL_CONTROLLER_BUTTON_X, 0x63}, {SDL_CONTROLLER_BUTTON_Y, 0x76},
        {SDL_CONTROLLER_BUTTON_BACK, 0x60}, {SDL_CONTROLLER_BUTTON_GUIDE, 0x61}, {SDL_CONTROLLER_BUTTON_START, 0x62},
        {SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, 0x64},
    };
    if (e->type == SDL_CONTROLLERDEVICEADDED) {
        if (!s->pad) s->pad = SDL_GameControllerOpen(e->cdevice.which);
        return 1;
    }
    if (e->type == SDL_CONTROLLERDEVICEREMOVED) {
        if (s->pad && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(s->pad)) == e->cdevice.which) {
            SDL_GameControllerClose(s->pad);
            s->pad = NULL;
            if (s->buttons & PAD_FIRE) {
                s->buttons &= ~PAD_FIRE;
                if (s->m) mc_machine_mouse(s->m, 0, 0, s->buttons != 0);
            }
        }
        return 1;
    }
    if (e->type != SDL_CONTROLLERBUTTONDOWN && e->type != SDL_CONTROLLERBUTTONUP) return 0;
    if (s->config_open || !pad_console(s)) return 1;
    const int down = e->type == SDL_CONTROLLERBUTTONDOWN;
    if (e->cbutton.button == SDL_CONTROLLER_BUTTON_LEFTSHOULDER) {
        s->buttons = down ? s->buttons | PAD_FIRE : s->buttons & ~PAD_FIRE;
        mc_machine_mouse(s->m, 0, 0, s->buttons != 0);
        return 1;
    }
    for (size_t i = 0; i < sizeof map / sizeof *map; ++i)
        if (map[i].button == e->cbutton.button) queue_key(s, map[i].key, down);
    return 1;
}
/* Once a frame: the left stick as the trackball, up to 12 counts a frame at
 * full deflection, nothing inside the dead zone. */
static void pad_frame(shell *s) {
    if (!s->pad || s->config_open || !pad_console(s)) return;
    const int x = SDL_GameControllerGetAxis(s->pad, SDL_CONTROLLER_AXIS_LEFTX);
    const int y = SDL_GameControllerGetAxis(s->pad, SDL_CONTROLLER_AXIS_LEFTY);
    const int dx = x > 8000 || x < -8000 ? x * 12 / 32768 : 0, dy = y > 8000 || y < -8000 ? y * 12 / 32768 : 0;
    if (dx || dy) mc_machine_mouse(s->m, dx, dy, s->buttons != 0);
}

/* One SDL event; 0 ends the run. */
static int event(shell *s, const SDL_Event *e) {
    mc_machine *m = s->m;
    if (e->type == SDL_MOUSEMOTION)
        mouse_trace_state(s, "SDL_MOTION", e->motion.xrel, e->motion.yrel, s->buttons);
    else if (e->type == SDL_MOUSEBUTTONDOWN || e->type == SDL_MOUSEBUTTONUP)
        mouse_trace_state(s, "SDL_BUTTON", e->button.x, e->button.y, (unsigned)e->button.button | e->type << 8);
    else if (e->type == SDL_WINDOWEVENT)
        mouse_trace_state(s, "SDL_WINDOW", e->window.data1, e->window.data2, e->window.event);
    if (e->type == SDL_CONTROLLERDEVICEADDED || e->type == SDL_CONTROLLERDEVICEREMOVED ||
        e->type == SDL_CONTROLLERBUTTONDOWN || e->type == SDL_CONTROLLERBUTTONUP)
        return pad_event(s, e);
    if (e->type == config_event && config_event) {
        if (s->config_open) cfg_app_close(s->app);
        else config_open(s);
        return 1;
    }
    if (e->type == SDL_QUIT) {
        /* Unsaved changes are asked about first. */
        if (s->app && !s->asked && cfg_app_dirty(s->app) && s->app->dialog != CFG_DIALOG_QUIT) {
            config_open(s);
            cfg_app_quit(s->app);
            return 1;
        }
        return close_request(s);
    }
    if (e->type == SDL_DROPFILE) {
        if (e->drop.file) { drop(s, e->drop.file); SDL_free(e->drop.file); }
        return 1;
    }
    if (s->config_open) {
        if (e->type == SDL_KEYDOWN && e->key.keysym.sym == SDLK_RETURN && (SDL_GetModState() & KMOD_ALT)) {
            mc_video_set_fullscreen(s->video, !mc_video_fullscreen(s->video));
            return 1;
        }
        /* Each press, release, key, text and wheel turn is a frame of its
         * own, then one more (a chosen menu item reaches its pop-up in the
         * next frame); the main loop shows the result at its next refresh.
         * Shown here, a burst of wheel events would present many frames
         * within one refresh, which some drivers (Remote Desktop's) show as
         * a black flash. */
        ui_input in;
        memset(&in, 0, sizeof in);
        config_event_input(s, e, &in);
        if (in.pressed || in.released || in.key || in.text[0] || in.wheel) {
            config_mouse(s, &in);
            config_draw(s, &in);
            ui_input idle;
            memset(&idle, 0, sizeof idle);
            idle.x = in.x;
            idle.y = in.y;
            config_draw(s, &idle);
        }
        return 1;
    }
    if (!m) {
        if (e->type == SDL_WINDOWEVENT) s->logo_shown = 0;
        else if (e->type == SDL_MOUSEBUTTONDOWN && s->session) config_open(s);
        return 1;
    }
    /* While the shutdown's own inputs go in, the host's wait. */
    if (s->closing && mc_shutdown_injecting(&s->sd) &&
        (e->type == SDL_KEYDOWN || e->type == SDL_KEYUP || e->type == SDL_MOUSEMOTION || e->type == SDL_MOUSEBUTTONDOWN ||
         e->type == SDL_MOUSEBUTTONUP))
        return 1;
    switch (e->type) {
    case SDL_KEYDOWN:
    case SDL_KEYUP: {
        int down = e->type == SDL_KEYDOWN;
        SDL_Keymod mod = (SDL_Keymod)e->key.keysym.mod;
        SDL_Scancode sc = e->key.keysym.scancode;
        if (down && e->key.repeat) return 1;
        if (down && (mod & KMOD_CTRL) && (mod & KMOD_ALT)) capture(s, 0);
        if (sc == SDL_SCANCODE_R && (mod & KMOD_CTRL) && (mod & KMOD_ALT)) {
            if (down && mc_machine_reinsert(m)) mc_osd_show(&s->osd, SDL_GetTicks(), "Disk reinserted");
            return 1;
        }
        /* The programmer's switch: Ctrl+Alt+F7 the interrupt button, F8
         * the reset button, pressed while the key is down. */
        if ((mod & KMOD_CTRL) && (mod & KMOD_ALT) && (sc == SDL_SCANCODE_F7 || sc == SDL_SCANCODE_F8)) {
            const mc_control which = sc == SDL_SCANCODE_F7 ? MC_CONTROL_INTERRUPT : MC_CONTROL_RESET;
            if (mc_machine_control(m, which, down) >= 0 && down)
                mc_osd_show(&s->osd, SDL_GetTicks(), "%s", which == MC_CONTROL_INTERRUPT ? "Interrupt" : "Reset");
            return 1;
        }
        /* The machine's own controls (docs/NOTES.md, "Machine
         * controls"): Ctrl+Alt+K turns the keyswitch On, Secure, Off and
         * On again; Ctrl+Alt+F1/F2 brightness down and up, F3/F4 contrast,
         * F5/F6 volume (front-panel buttons). */
        if ((mod & KMOD_CTRL) && (mod & KMOD_ALT) &&
            (sc == SDL_SCANCODE_K || (sc >= SDL_SCANCODE_F1 && sc <= SDL_SCANCODE_F6))) {
            if (!down) return 1;
            if (sc == SDL_SCANCODE_K) {
                static const char *const names[3] = {"Off", "On", "Secure"};
                const int now = mc_machine_control(m, MC_CONTROL_KEYSWITCH, -1);
                if (now >= 0) {
                    const int next = mc_machine_control(m, MC_CONTROL_KEYSWITCH, now == 1 ? 2 : now == 2 ? 0 : 1);
                    mc_osd_show(&s->osd, SDL_GetTicks(), "Keyswitch: %s", names[next]);
                }
            } else {
                static const char *const names[3] = {"Brightness", "Contrast", "Volume"};
                static const mc_control controls[3] = {MC_CONTROL_BRIGHTNESS, MC_CONTROL_CONTRAST, MC_CONTROL_VOLUME};
                const int which = (int)(sc - SDL_SCANCODE_F1) / 2, up = (int)(sc - SDL_SCANCODE_F1) % 2;
                if (mc_machine_control(m, controls[which], up ? 1 : -1) >= 0)
                    mc_osd_show(&s->osd, SDL_GetTicks(), "%s %s", names[which], up ? "up" : "down");
            }
            return 1;
        }
        /* Alt+Enter stays the window's: full screen and back. */
        if (sc == SDL_SCANCODE_RETURN && (mod & KMOD_ALT)) {
            if (down) mc_video_set_fullscreen(s->video, !mc_video_fullscreen(s->video));
            return 1;
        }
        int code = mc_desktop_adb_key(sc, s->live.command_key);
        if (code >= 0 && s->pressed[code] != (uint8_t)down) {
            s->pressed[code] = (uint8_t)down;
            return queue_key(s, (uint8_t)code, down);
        }
        return 1;
    }
    case SDL_MOUSEMOTION:
        if (pointing_changed(s)) return capture(s, 1);
        if (s->absolute_mouse) {
            mc_video_to_screen(s->video, e->motion.x, e->motion.y, &s->absolute_x, &s->absolute_y);
            if (!absolute_move(s)) return capture(s, 1);
        } else if (SDL_GetRelativeMouseMode()) mc_machine_mouse(m, e->motion.xrel, e->motion.yrel, s->buttons != 0);
        return 1;
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP: {
        if (pointing_changed(s) && !capture(s, 1)) return 0;
        if (!s->absolute_mouse && !SDL_GetRelativeMouseMode()) {
            if (s->hidden || e->type != SDL_MOUSEBUTTONDOWN) return 1;
            int gx, gy;
            if (!mc_video_to_screen(s->video, e->button.x, e->button.y, &gx, &gy) ||
                !(SDL_GetWindowFlags(mc_video_window(s->video)) & SDL_WINDOW_INPUT_FOCUS)) return 1;
            if (!capture(s, 1)) return 0;
            /* The capturing click reaches the guest too, on RDP and locally. */
        }
        if (e->button.button != SDL_BUTTON_LEFT && e->button.button != SDL_BUTTON_RIGHT) return 1;
        /* The Macintosh's one button is down while any button that is not
         * disabled is; a control-click button also holds Control. */
        unsigned index = e->button.button == SDL_BUTTON_RIGHT, bit = 1u << index;
        int down = e->type == SDL_MOUSEBUTTONDOWN;
        uint32_t role = s->live.mouse[index];
        if (role == MC_MOUSE_DISABLED || ((s->buttons & bit) != 0) == down) return 1;
        s->buttons = down ? s->buttons | bit : s->buttons & ~bit;
        if (role == MC_MOUSE_CONTROL_CLICK && !queue_key(s, 0x36, down)) return 0;
        if (s->absolute_mouse) {
            mc_video_to_screen(s->video, e->button.x, e->button.y, &s->absolute_x, &s->absolute_y);
            if (!absolute_move(s)) return capture(s, 1);
            if (s->absolute_mouse == 2) click_push(s);
        } else if (!mc_machine_mouse(m, 0, 0, s->buttons != 0)) { snprintf(s->error, sizeof s->error, "Host mouse queue overflow"); return 0; }
        return 1;
    }
    case SDL_WINDOWEVENT:
        if (e->window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
            capture(s, 0);
            s->buttons = 0;
            mc_machine_mouse(m, 0, 0, 0);
            for (unsigned k = 0; k < 128; ++k)
                if (s->pressed[k]) { s->pressed[k] = 0; if (!queue_key(s, (uint8_t)k, 0)) return 0; }
        }
        return 1;
    default: return 1;
    }
}

/* --- the run ------------------------------------------------------------------------------------- */

static int run(mc_machine *m, const mc_config *config, const char *profile, const char *problem, const mc_desktop_options *options,
               const mc_desktop_session *session, char *error, size_t capacity) {
    shell *s = calloc(1, sizeof *s);
    if (!s) { snprintf(error, capacity, "Out of memory"); return 1; }
    s->m = m;
    s->owns = session != NULL;
    s->live = s->started = *config;
    s->has_profile = profile != NULL;
    snprintf(s->profile, sizeof s->profile, "%s", profile ? profile : "");
    s->options = options;
    s->session = session;
    s->hidden = options->hidden;
    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_TIMER)) {
        snprintf(error, capacity, "SDL: %s", SDL_GetError());
        if (s->owns && m) mc_machine_destroy(m);
        free(s);
        return 1;
    }
    /* A game controller is optional: without the subsystem none is seen. */
    if (!s->hidden) SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER);
    if (session) config_event = SDL_RegisterEvents(1);
    screen_size(s);
    mc_settings settings;
    memset(&settings, 0, sizeof settings);
    settings.window_scale = options->window_scale > 0 ? options->window_scale : 1;
    settings.aspect = MC_ASPECT_SQUARE;
    settings.fullscreen = config->fullscreen && !s->hidden;
    /* A hidden window never reaches a display: nothing to wait for. */
    settings.vsync = config->vsync && !s->hidden;
    s->video = mc_video_open(TITLE_FREE, &settings, (int)s->width, (int)s->height, (int)s->width, (int)s->height, s->hidden != 0, error,
                             capacity);
    if (!s->video) {
        if (s->owns && m) mc_machine_destroy(m);
        SDL_Quit();
        free(s);
        return 1;
    }
#ifndef _WIN32
    /* Windows takes the window's icon from the executable's (assets/livadix.ico). */
    mc_video_set_icon(s->video, MC_ICON_ARGB, MC_ICON_SIZE);
#endif
    apply_look(s);
    int status = 0;
    s->pixels = calloc((size_t)1920 * 1080, sizeof *s->pixels);
    if (!s->pixels) { snprintf(error, capacity, "Cannot allocate the display surface"); status = 1; }
    if (!status && options->wav && !mc_wav_open(&s->wav, options->wav)) { snprintf(error, capacity, "Cannot write %s", options->wav); status = 1; }
    if (!status && s->m && !attach(s, error, capacity)) status = 1;
    if (status) {
        if (options->wav) mc_wav_close(&s->wav);
        if (s->owns && s->m) mc_machine_destroy(s->m);
        free(s->pixels);
    free(s->logo);
    free(s->overlay);
        mc_video_close(s->video);
        SDL_Quit();
        free(s);
        return 1;
    }
    SDL_EventState(SDL_DROPFILE, SDL_ENABLE);
    mouse_trace_open(s);
    if (!s->hidden) {
        console_open = 1;
#ifdef _WIN32
        SetConsoleCtrlHandler(console_event, TRUE);
#else
        console_handler(1);
#endif
        if (session) hook_install(s->video);
    }
    if (session && session->script) {
        s->script.f = fopen(session->script, "rb");
        if (!s->script.f) { snprintf(s->error, sizeof s->error, "Cannot read the script %s", session->script); status = 1; }
    }
    /* A machine that could not start: the configuration window says why. */
    if (session && !s->m) {
        printf("Configuration: the Macintosh could not start: %s\n", problem ? problem : "");
        if (config_ready(s)) {
            config_open(s);
            if (profile) cfg_app_select(s->app, profile);
            cfg_app_set_problem(s->app, problem);
            cfg_app_set_running(s->app, NULL);
            ui_input idle;
            memset(&idle, 0, sizeof idle);
            config_frame(s, &idle);
        } else status = 1;
    }

    const uint64_t freq = SDL_GetPerformanceFrequency();
    /* Pacing is in guest time (C15M periods), which device accesses
     * lengthen beyond the CPU's cycles. */
    const uint64_t catch_up = (uint64_t)MC_C15M_HZ * CATCH_UP_MS / 1000u;
    const uint64_t slice = MC_C15M_HZ / 1000u;                     /* at most 1 ms of guest time per run call */
    double rate = s->m ? (double)mc_machine_rate(s->m) : 1e7;      /* guest CPU cycles per second */
    uint64_t slice_cycles = (uint64_t)(rate / 1000.0) + 1;
    uint64_t last_present = 0;
    uint64_t anchor = SDL_GetPerformanceCounter(), anchor_done = s->m ? mc_machine_time(s->m) : 0;
    uint64_t next_frame = anchor, frames = 0, last_serial = UINT64_MAX, ticks = 0;
    unsigned close_next = 0, key_next = 0;
    /* With vsync a present can wait for the display. A wait ends at a
     * vertical blank: frames are then presented just after the blank that
     * shows the previous one, so they rarely wait. The display's interval
     * is its refresh rate as the system reports it (60 Hz when it reports
     * none or an implausible one), never one guessed from the gaps between
     * waits: presents that wait only now and then (a large picture's
     * upload, a mode change, a remote desktop's frame) are far apart, and
     * taking their gap for the interval held every later present to it,
     * a few frames a second, so the Macintosh's drawing appeared slowly
     * however fast it ran. The time a present does wait is made up by the
     * pacing below. */
    uint64_t blank = 0, target = 0;
    uint64_t display = freq / 60;
    {
        SDL_DisplayMode mode;
        if (!SDL_GetWindowDisplayMode(mc_video_window(s->video), &mode) && mode.refresh_rate >= 24 && mode.refresh_rate <= 360)
            display = freq / (uint64_t)mode.refresh_rate;
    }
    /* The guest time the presents took since the Macintosh last caught up
     * with wall time, at most PRESENT_CATCH_UP_MS: made up in full, beyond
     * the catch-up of other lags. The window loop presents on the thread
     * that runs the Macintosh, and a present can wait longer than the
     * catch-up (for a remote desktop's or a compositor's frame, while the
     * screen changes much, as when the Macintosh draws windows): dropping
     * that time made the Macintosh run below its clock exactly while it
     * drew. */
    const uint64_t present_cap = (uint64_t)MC_C15M_HZ * PRESENT_CATCH_UP_MS / 1000u;
    uint64_t owed = 0;
    int running = !status;
    stats st;
    memset(&st, 0, sizeof st);
    {
        const char *path = getenv("LIVADIX_STATS");
        if (path && *path && (st.f = fopen(path, "w")) != NULL) {
            st.t0 = st.last = SDL_GetPerformanceCounter();
            st.guest0 = s->m ? mc_machine_time(s->m) : 0;
        }
    }
    mc_framebuffer fb;
    memset(&fb, 0, sizeof fb);
    while (running) {
        SDL_Event e;
        while (running && SDL_PollEvent(&e)) running = event(s, &e);
        if (!running) { if (s->error[0]) status = 1; break; }
        /* The script, at frame boundaries. */
        if (s->script.f && !s->script.done && !s->script_wait && !script_step(s)) { status = 1; snprintf(s->error, sizeof s->error, "The script failed"); break; }
        /* What the configuration window asked for. */
        if (s->quit) {
            /* Quitting from the configuration window: as closing the window. */
            s->quit = 0;
            if (s->config_open && s->m) config_close(s);
            if (!close_request(s)) break;
        }
        if (s->stop) {
            /* Stop: as closing the window, but the end of the shutdown stops. */
            s->stop = 0;
            s->restart_queued = 0;
            if (s->m) {
                if (s->config_open) config_close(s);
                if (close_request(s)) s->stopping = 1;
                else stopped(s);
            }
        }
        if (s->restart && s->m && !s->closing && s->restart_has_profile == s->has_profile &&
            (!s->has_profile || !strcmp(s->restart_profile, s->profile))) {
            /* Restart: the Mac OS shuts down first, as for Stop; the profile
             * starts again when it is off (shutdown_done). Without a System
             * to shut down, or with close_action quit, it starts at once. */
            s->resume = 0;
            if (s->config_open) config_close(s);
            if (close_request(s)) { s->restart = 0; s->stopping = 1; s->restart_queued = 1; }
        }
        if (s->restart) {
            s->restart = 0;
            s->resume = 0;
            const mc_config want = s->restart_config;
            const char *name = s->restart_has_profile ? s->restart_profile : NULL;
            char kept[MC_CONFIG_NAME];
            snprintf(kept, sizeof kept, "%s", name ? name : "");
            start_machine(s, &want, name ? kept : NULL);
            if (s->m) {
                rate = (double)mc_machine_rate(s->m);
                slice_cycles = (uint64_t)(rate / 1000.0) + 1;
                anchor = SDL_GetPerformanceCounter();
                anchor_done = mc_machine_time(s->m);
                next_frame = anchor;
                owed = 0;
                target = 0;
                last_serial = UINT64_MAX;
            }
        }
        if (s->resume) {
            s->resume = 0;
            if (s->config_open) config_apply(s);
            if (s->m && !s->config_open) {
                anchor = SDL_GetPerformanceCounter();
                anchor_done = mc_machine_time(s->m);
                next_frame = anchor;
                last_serial = UINT64_MAX;
                if (s->started.model != s->live.model || mc_machine_rate(s->m) != (uint64_t)rate) {
                    rate = (double)mc_machine_rate(s->m);
                    slice_cycles = (uint64_t)(rate / 1000.0) + 1;
                }
            }
        }
        const uint64_t frame_ticks = freq / s->live.refresh;
        const uint64_t frame_guest = MC_C15M_HZ / s->live.refresh;
        if (s->config_open || !s->m) {
            /* The Macintosh paused under the configuration window. */
            uint64_t now = SDL_GetPerformanceCounter();
            if (!s->hidden && now < next_frame) { SDL_Delay(1); continue; }
            next_frame = now + freq / 60;
            if (s->config_open) {
                ui_input idle;
                memset(&idle, 0, sizeof idle);
                config_mouse(s, &idle);
                config_frame(s, &idle);
            } else if (!s->logo_shown) logo_present(s);
            if (s->script_wait) --s->script_wait;
            ++ticks;
            if (options->frames && ++frames >= options->frames) running = 0;
            continue;
        }
        pad_frame(s);
        while (s->read != s->write) {
            uint8_t k = s->pending[s->read];
            if (!mc_machine_key(s->m, k & 0x7f, !(k & 0x80))) break;
            s->read = (s->read + 1) & 1023;
        }
        if (s->closing && SDL_GetTicks() - s->close_ms >= s->live.close_timeout * 1000u) {
            printf("Close: the Macintosh did not shut down within %u s; %s at once\n", (unsigned)s->live.close_timeout,
                   s->stopping ? "stopped" : "quit");
            if (s->stopping) { shutdown_done(s); continue; }
            break;
        }
        /* Pacing: run what wall time owes, catching up at most 50 ms beyond
         * the time the presents took. */
        uint64_t now = SDL_GetPerformanceCounter();
        uint64_t done = mc_machine_time(s->m);
        uint64_t due = anchor_done + (uint64_t)((double)(now - anchor) * MC_C15M_HZ / (double)freq);
        if (due > done + catch_up + owed) {
            if (options->dropped) *options->dropped += due - done;
            st.dropped += due - done;
            anchor = now; anchor_done = done; due = done;
        }
        /* Unthrottled: a frame's worth of guest time, then the frame. */
        if (options->unthrottled) {
            if (target <= done) target = done + frame_guest;
            due = target;
        }
        if (due > done) {
            /* Up to 1 ms of guest time; the cycle cap keeps a run
             * call short whatever the device accesses cost. */
            mc_run_status result = mc_machine_run(s->m, slice_cycles, due - done < slice ? due : done + slice);
            if (mc_machine_time(s->m) >= due) owed = 0;   /* caught up */
            if (result == MC_RUN_HALTED) {
                if (s->closing)
                    printf("Close: the Macintosh switched itself off after %.1f s (%.1f s of its own time)\n",
                           (SDL_GetTicks() - s->close_ms) / 1000.0, (double)(mc_machine_time(s->m) - s->close_time) / MC_C15M_HZ);
                else printf("The Macintosh switched itself off\n");
                if (s->stopping) { shutdown_done(s); continue; }
                break;
            }
            if (result != MC_RUN_OK) { snprintf(s->error, sizeof s->error, "%s", mc_machine_error(s->m)); status = 1; break; }
            if (options->wav) mc_wav_pump(&s->wav, s->m);
            else if (!mc_host_audio_pump(&s->audio, s->m)) { snprintf(s->error, sizeof s->error, "Sound output: %s", SDL_GetError()); status = 1; break; }
            mc_host_serial_pump(&s->serial, s->m);
            mc_net_pump(s->net, s->m);
            click_step(s);
            if (s->closing) {
                mc_shutdown_step(&s->sd, s->m);
                if (mc_shutdown_unanswered(&s->sd)) {
                    printf("Close: the System brought up no shut down dialog for the power key; %s at once\n",
                           s->stopping ? "stopped" : "quit");
                    if (s->stopping) { shutdown_done(s); continue; }
                    break;
                }
                /* No soft power: the System's final screen ("It is now safe
                 * to switch off your Macintosh") is the end (shutdown.h). */
                if (mc_shutdown_final(&s->sd, s->m)) {
                    printf("Close: the Macintosh shut down after %.1f s (%.1f s of its own time); it has no soft power: "
                           "its final screen is the end\n",
                           (SDL_GetTicks() - s->close_ms) / 1000.0, (double)(mc_machine_time(s->m) - s->close_time) / MC_C15M_HZ);
                    if (s->stopping) { shutdown_done(s); continue; }
                    break;
                }
            }
            if (options->after_run && !options->after_run(options->ctx, s->m)) running = 0;
            if (options->unthrottled && mc_machine_time(s->m) < due) continue;
        }
        now = SDL_GetPerformanceCounter();
        stats_tick(&st, s->m, now, freq);
        if (options->unthrottled) next_frame = now;
        if (now < next_frame) {
            if (due <= done && next_frame - now > freq / 1000) SDL_Delay(1);
            continue;
        }
        /* The Macintosh first catches up: while it is more than a frame
         * behind wall time (after a present that took longer than a frame,
         * as a remote desktop's, a compositor's or a software renderer's
         * can, full screen), it runs instead of the next present. Presenting
         * on schedule regardless left it one slice of guest time per
         * present: it drew at a fraction of its speed whatever its clock.
         * A host that cannot keep up still shows a frame every
         * PRESENT_MAX_FRAMES frames. */
        if (!options->unthrottled && due > mc_machine_time(s->m) + frame_guest &&
            now - last_present < frame_ticks * PRESENT_MAX_FRAMES) {
            if (st.f && st.skip_at != next_frame) { st.skip_at = next_frame; ++st.skipped; }
            continue;
        }
        last_present = now;
        next_frame = now - next_frame < frame_ticks ? next_frame + frame_ticks : now + frame_ticks;
        /* Checks: keys and close requests at given frames. */
        ++ticks;
        if (s->script_wait) --s->script_wait;
        while (key_next < options->key_count && ticks >= options->key_at[key_next]) {
            const uint8_t code = options->key_code[key_next++];
            if (!queue_key(s, code & 0x7Fu, !(code & 0x80u))) running = 0;
        }
        while (close_next < options->close_count && ticks >= options->close_at[close_next]) {
            ++close_next;
            if (!close_request(s)) running = 0;
        }
        if (!running) break;
        if (s->closing) mc_osd_show(&s->osd, SDL_GetTicks(), "%s", s->restart_queued ? "Restarting..." : s->stopping ? "Shutting down..." :
                                                                 "Shutting down... close again to quit now");
        /* Unthrottled, the screen is drawn every 30th frame (and the last):
         * the time goes to the Macintosh. */
        if (options->unthrottled && ++frames % 30 != 0 && (!options->frames || frames < options->frames)) continue;
        if (options->unthrottled) frames--;
        const uint64_t convert = SDL_GetPerformanceCounter();
        if (!mc_machine_framebuffer(s->m, &fb)) { memset(&fb, 0, sizeof fb); fb.width = s->width; fb.height = s->height; }
        if (fb.pixels && (fb.serial != last_serial || s->osd.text[0])) {
            mc_framebuffer_argb(&fb, s->pixels);
            last_serial = fb.serial;
        } else if (!fb.pixels) memset(s->pixels, 0, (size_t)fb.width * fb.height * sizeof *s->pixels);
        s->width = fb.width;
        s->height = fb.height;
        mc_osd_draw(&s->osd, SDL_GetTicks(), s->pixels, (int)fb.width, (int)fb.height);
        if (s->osd.text[0] && (int32_t)(s->osd.until_ms - SDL_GetTicks()) <= 0) { s->osd.text[0] = 0; last_serial = UINT64_MAX; }
        uint64_t start = SDL_GetPerformanceCounter();
        mc_video_set_geometry(s->video, (int)fb.width, (int)fb.height);
        mc_video_present(s->video, s->pixels, (int)fb.width, (int)fb.height);
        if (options->after_present) options->after_present(options->ctx, s->m, s->pixels, (int)fb.width, (int)fb.height);
        uint64_t end = SDL_GetPerformanceCounter();
        if (options->presents) ++*options->presents;
        if (st.f) {
            const size_t n = (size_t)fb.width * fb.height;
            if (n != st.previous_n) { free(st.previous); st.previous = calloc(n, sizeof *s->pixels); st.previous_n = st.previous ? n : 0; ++st.changed; }
            else if (memcmp(st.previous, s->pixels, n * sizeof *s->pixels)) ++st.changed;
            if (st.previous) memcpy(st.previous, s->pixels, n * sizeof *s->pixels);
            ++st.presents;
            st.convert += start - convert;
            st.present += end - start;
            if (end - start > st.present_max) st.present_max = end - start;
        }
        if (options->present_ticks) *options->present_ticks += end - convert;
        owed += (uint64_t)((double)(end - convert) * MC_C15M_HZ / (double)freq);
        if (owed > present_cap) owed = present_cap;
        if (s->live.vsync && !s->hidden && (end - start) * 1000u > freq) blank = end;
        /* Aligned to the last wait seen within a second; without one the
         * fixed schedule stands. */
        if (s->live.vsync && !s->hidden && blank && end - blank < freq) {
            uint64_t shown = blank + ((end - blank) / display + 1) * display + freq / 1000u;
            if (next_frame < shown) next_frame = shown;
        }
        if (options->frames && ++frames >= options->frames) running = 0;
    }
    if (status == 0 && s->script.f && !s->script.done && !s->quit) {
        /* The run ended before the script did. */
        snprintf(s->error, sizeof s->error, "The run ended at line %d of the script", s->script.line);
        status = 1;
    }
    if (status == 0 && options->screenshot && s->m) {
        /* The guest's frame itself, or with a look, the picture as shown. */
        const int plain = s->live.look == MC_LOOK_PLAIN && s->live.monitor == MC_MONITOR_COLOR;
        if (mc_machine_framebuffer(s->m, &fb)) mc_framebuffer_argb(&fb, s->pixels);
        if (s->config_open) config_close(s);
        mc_video_request_shot(s->video, options->screenshot, plain);
        mc_video_present(s->video, s->pixels, (int)fb.width, (int)fb.height);
        char saved[1024];
        if (!mc_video_take_shot_result(s->video, saved, sizeof saved) || !saved[0]) {
            snprintf(s->error, sizeof s->error, "Cannot save the screenshot %s", options->screenshot);
            status = 1;
        }
    }
    if (options->wav && !mc_wav_close(&s->wav) && !status) {
        snprintf(s->error, sizeof s->error, "Cannot write %s", options->wav);
        status = 1;
    }
    if (status) snprintf(error, capacity, "%s", s->error[0] ? s->error : error);
    if (st.f) fclose(st.f);
    free(st.previous);
    fflush(stdout);
    if (console_open) {
        console_open = 0;
#ifdef _WIN32
        SetConsoleCtrlHandler(console_event, FALSE);
#else
        console_handler(0);
#endif
    }
    hook_remove();
    if (s->script.f) fclose(s->script.f);
    detach(s);
    if(s->save_failed){status=1;snprintf(error,capacity,"%s",s->error);}
    if (s->app) {
        free(s->app->ui.canvas.px);
        cfg_app_free(s->app);
        free(s->app);
    }
    free(s->pixels);
    free(s->logo);
    free(s->overlay);
    if (s->pad) SDL_GameControllerClose(s->pad);
    capture(s, 0);
    if (s->remote_cursor) SDL_FreeCursor(s->remote_cursor);
    mouse_trace_close(s);
    mc_video_close(s->video);
    SDL_Quit();
    free(s);
    return status;
}

int mc_desktop_run(mc_machine *m, const mc_config *config, const mc_desktop_options *options, char *error, size_t capacity) {
    return run(m, config, config->profile[0] ? config->profile : NULL, NULL, options, NULL, error, capacity);
}
int mc_desktop_session_run(mc_machine *machine, const mc_config *config, const char *profile, const char *problem,
                           const mc_desktop_options *options, const mc_desktop_session *session, char *error, size_t capacity) {
    return run(machine, config, profile, problem, options, session, error, capacity);
}
