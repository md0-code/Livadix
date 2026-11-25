/* The configuration window's widget toolkit: immediate-mode controls drawn in software
 * into a 32-bit canvas in the manner of the Mac OS 8 "Platinum" appearance
 * (gray 3D controls, pinstriped title bars, the default button's ring,
 * pop-up menus, tabs, list boxes with scroll bars, edit fields, sliders,
 * disclosure triangles, alerts). Livadix's own design and code; the fonts and
 * icons are Livadix's own (ui_font.c, ui_icons.c).
 *
 * Each frame the application calls ui_begin with the frame's input, draws
 * its controls (each returns what the input did to it), and ui_end. A
 * control is known by an id that stays the same from frame to frame. */

#ifndef MC_UI_H
#define MC_UI_H

#include <stddef.h>
#include <stdint.h>

typedef struct ui_rect { int x, y, w, h; } ui_rect;

typedef struct ui_canvas {
    uint32_t *px;              /* 0x00RRGGBB */
    int w, h;
    ui_rect clip;
} ui_canvas;

/* The Platinum palette. */
enum {
    UI_BLACK = 0x000000, UI_WHITE = 0xFFFFFF,
    UI_FACE = 0xDDDDDD,        /* window content */
    UI_LIGHT = 0xEEEEEE, UI_SHADE = 0xBBBBBB, UI_SHADOW = 0x888888, UI_DARK = 0x555555,
    UI_DISABLED = 0x999999,
    UI_ACCENT = 0xCCCCFF,      /* selection (lavender) */
    UI_ACCENT_DARK = 0x6666CC, /* focus ring, slider fill */
    UI_OK = 0x228822, UI_WARN = 0xCC8800, UI_ERROR = 0xCC2222
};

void ui_pixel(ui_canvas *c, int x, int y, uint32_t color);
void ui_fill(ui_canvas *c, int x, int y, int w, int h, uint32_t color);
void ui_frame(ui_canvas *c, int x, int y, int w, int h, uint32_t color);
/* A raised (or sunken) 3D edge inside the rectangle: light top-left, shadow bottom-right. */
void ui_bevel(ui_canvas *c, ui_rect r, int sunken);
void ui_set_clip(ui_canvas *c, ui_rect r);
void ui_clear_clip(ui_canvas *c);

/* Fonts (ui_font.c). */
enum { UI_FONT_SYSTEM, UI_FONT_SMALL, UI_FONT_SMALL_BOLD };
typedef struct ui_font ui_font;
const ui_font *ui_font_get(int which);
int ui_font_height(const ui_font *f);
int ui_font_ascent(const ui_font *f);
int ui_text_width(const ui_font *f, const char *utf8);
int ui_text_width_n(const ui_font *f, const char *utf8, size_t bytes);
/* Draws text with its top at y; returns the x after it. */
int ui_draw_text(ui_canvas *c, const ui_font *f, int x, int y, const char *utf8, uint32_t color);

/* Icons (ui_icons.c). */
enum { UI_ICON_HARD_DISK, UI_ICON_FLOPPY, UI_ICON_CD, UI_ICON_MAC, UI_ICON_OK, UI_ICON_WARNING, UI_ICON_STOP, UI_ICON_NOTE,
       UI_ICON_FOLDER, UI_ICON_DOCUMENT, UI_ICON_COUNT };
/* 16 x 16 (32 x 32 for UI_ICON_STOP, UI_ICON_NOTE and UI_ICON_WARNING at
 * large) at x, y. */
void ui_icon(ui_canvas *c, int icon, int x, int y, int large);

/* Keys the toolkit knows. */
enum { UI_KEY_NONE, UI_KEY_TAB, UI_KEY_ENTER, UI_KEY_ESCAPE, UI_KEY_SPACE, UI_KEY_UP, UI_KEY_DOWN, UI_KEY_LEFT, UI_KEY_RIGHT,
       UI_KEY_HOME, UI_KEY_END, UI_KEY_BACKSPACE, UI_KEY_DELETE, UI_KEY_PAGE_UP, UI_KEY_PAGE_DOWN, UI_KEY_CHAR };
enum { UI_MOD_SHIFT = 1, UI_MOD_CTRL = 2, UI_MOD_ALT = 4 };

/* One frame's input. */
typedef struct ui_input {
    int x, y;                  /* the mouse, in canvas pixels */
    int pressed, released;     /* the left button went down / up this frame */
    int double_click;          /* this press is a double click */
    int wheel;                 /* scroll steps, positive up */
    int key, mods;             /* UI_KEY_*, UI_MOD_* */
    int ch;                    /* with UI_KEY_CHAR: the character, lower-case for Ctrl combinations */
    char text[32];             /* typed text, UTF-8 */
} ui_input;

#define UI_MENU_ITEMS 192
#define UI_HITS 512

typedef struct ui {
    ui_canvas canvas;
    ui_input in;
    int down;                  /* the left button is held */
    uint32_t active;           /* the control the button went down on */
    uint32_t focus;            /* the control with the keyboard */
    int focus_moved;           /* Tab moved the focus this frame */
    uint32_t order[256], last_order[256];
    int order_count, last_order_count;
    int blocked;               /* controls drawn now ignore input (under a dialog) */
    /* The open pop-up menu. */
    struct {
        uint32_t id;
        ui_rect at;            /* the menu itself */
        int count, hover, current, item_height;
        int first, visible;    /* a menu taller than the canvas scrolls: its first row shown, the rows shown */
        int scroll_tick;       /* frames the mouse has rested on a scroll arrow */
        int mouse_x, mouse_y;  /* where the mouse was last frame: the hover follows it only when it moves */
        char items[UI_MENU_ITEMS][96];
        uint8_t enabled[UI_MENU_ITEMS];
        uint32_t chosen_id;    /* a choice made, for the control to take */
        int chosen;
        int sticky;            /* opened by a click: the release does not choose */
        int open_x, open_y;    /* where the mouse opened it */
    } menu;
    /* Every control drawn this frame with its label, for scripts. */
    struct { char label[96]; ui_rect r; uint32_t id; int kind; } hits[UI_HITS];
    int hit_count;
    /* The edit field being typed into. */
    uint32_t edit_id;
    int caret;
    /* Scroll bar dragging. */
    int drag_offset;
    int list_clicked;          /* the row the last ui_list was clicked on this frame, -1 none */
} ui;

enum { UI_HIT_BUTTON, UI_HIT_CHECKBOX, UI_HIT_RADIO, UI_HIT_POPUP, UI_HIT_TAB, UI_HIT_ROW, UI_HIT_EDIT, UI_HIT_SLIDER,
       UI_HIT_DISCLOSURE, UI_HIT_MENU_ITEM, UI_HIT_TEXT };

void ui_begin(ui *u, const ui_input *in);
void ui_end(ui *u);
/* The id of a control from a name and an index. */
uint32_t ui_id(const char *name, int index);
void ui_register(ui *u, uint32_t id, ui_rect r, const char *label, int kind);
/* Moves the focus to the control (0: none). */
void ui_focus(ui *u, uint32_t id);
int ui_inside(ui_rect r, int x, int y);

/* The window: a Platinum document window filling r, its pinstriped title
 * bar with the title; returns its content area. */
ui_rect ui_window(ui *u, ui_rect r, const char *title);
/* A movable modal dialog's frame; returns its content area. */
ui_rect ui_dialog(ui *u, ui_rect r);
/* A group box with its title; returns its inside. */
ui_rect ui_group(ui *u, ui_rect r, const char *title);
void ui_label(ui *u, int x, int y, const char *text, int font, uint32_t color);
/* Text wrapped to width; returns the height used. */
int ui_wrapped(ui *u, ui_rect r, const char *text, int font, uint32_t color);

enum { UI_DEFAULT = 1, UI_DISABLED_FLAG = 2 };
int ui_button(ui *u, uint32_t id, ui_rect r, const char *label, int flags);
int ui_checkbox(ui *u, uint32_t id, int x, int y, const char *label, int *value, int disabled);
int ui_radio(ui *u, uint32_t id, int x, int y, const char *label, int selected, int disabled);
/* A pop-up menu: items, the selection (-1: none, shown as text), enabled
 * flags (NULL: all). name labels it for scripts. Returns 1 when a new item
 * was chosen. */
int ui_popup(ui *u, uint32_t id, ui_rect r, const char *name, const char *const *items, int count, int *selected,
             const uint8_t *enabled, const char *text_when_none);
/* Scrolls the open menu so its item labelled label is shown (for scripts,
 * as the mouse would); 0 when no item has that label. */
int ui_menu_reveal(ui *u, const char *label);
int ui_tabs(ui *u, uint32_t id, ui_rect r, const char *const *labels, int count, int *current, ui_rect *panel);
/* A list box: rows drawn by draw_row; returns 1 when the selection changed
 * (and sets *activated on a double click or Enter). */
typedef void (*ui_row_fn)(void *ctx, ui *u, ui_rect r, int index, int selected);
int ui_list(ui *u, uint32_t id, ui_rect r, int row_height, int count, int *selected, int *scroll, ui_row_fn draw_row, void *ctx,
            const char *const *row_labels, int *activated);
/* An edit field; returns 1 when the text changed. */
int ui_edit(ui *u, uint32_t id, ui_rect r, char *text, size_t capacity, int disabled);
/* A slider over 0..1; returns 1 when moved. mark: a tick at that place (<0: none). */
int ui_slider(ui *u, uint32_t id, ui_rect r, double *value, double mark, int disabled);
int ui_disclosure(ui *u, uint32_t id, int x, int y, const char *label, int *open);
/* A text field's look without the editing (a read-only value). */
void ui_field(ui *u, ui_rect r, const char *text);

#endif /* MC_UI_H */
