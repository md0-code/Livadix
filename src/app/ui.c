/* The configuration window's widget toolkit (ui.h): immediate-mode controls drawn in the
 * manner of the Mac OS 8 Platinum appearance. Livadix's own design and code. */
#include "ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MENU_SELECTED 0x333399

/* --- drawing ------------------------------------------------------------------------ */

void ui_pixel(ui_canvas *c, int x, int y, uint32_t color) {
    if (x < c->clip.x || y < c->clip.y || x >= c->clip.x + c->clip.w || y >= c->clip.y + c->clip.h) return;
    if (x < 0 || y < 0 || x >= c->w || y >= c->h) return;
    c->px[(size_t)y * c->w + x] = color;
}
void ui_fill(ui_canvas *c, int x, int y, int w, int h, uint32_t color) {
    int x0 = x > c->clip.x ? x : c->clip.x, y0 = y > c->clip.y ? y : c->clip.y;
    int x1 = x + w < c->clip.x + c->clip.w ? x + w : c->clip.x + c->clip.w;
    int y1 = y + h < c->clip.y + c->clip.h ? y + h : c->clip.y + c->clip.h;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > c->w) x1 = c->w;
    if (y1 > c->h) y1 = c->h;
    for (int j = y0; j < y1; ++j)
        for (int i = x0; i < x1; ++i) c->px[(size_t)j * c->w + i] = color;
}
void ui_frame(ui_canvas *c, int x, int y, int w, int h, uint32_t color) {
    if (w <= 0 || h <= 0) return;
    ui_fill(c, x, y, w, 1, color);
    ui_fill(c, x, y + h - 1, w, 1, color);
    ui_fill(c, x, y, 1, h, color);
    ui_fill(c, x + w - 1, y, 1, h, color);
}
void ui_bevel(ui_canvas *c, ui_rect r, int sunken) {
    const uint32_t a = sunken ? UI_SHADOW : UI_WHITE, b = sunken ? UI_WHITE : UI_SHADOW;
    ui_fill(c, r.x, r.y, r.w - 1, 1, a);
    ui_fill(c, r.x, r.y, 1, r.h - 1, a);
    ui_fill(c, r.x + 1, r.y + r.h - 1, r.w - 1, 1, b);
    ui_fill(c, r.x + r.w - 1, r.y + 1, 1, r.h - 1, b);
}
void ui_set_clip(ui_canvas *c, ui_rect r) { c->clip = r; }
void ui_clear_clip(ui_canvas *c) { c->clip = (ui_rect){0, 0, c->w, c->h}; }
/* A rectangle with its four corner pixels left out: the Platinum controls'
 * rounded corners. */
static void rounded(ui_canvas *c, ui_rect r, uint32_t outline, uint32_t face) {
    ui_fill(c, r.x + 1, r.y + 1, r.w - 2, r.h - 2, face);
    ui_fill(c, r.x + 2, r.y, r.w - 4, 1, outline);
    ui_fill(c, r.x + 2, r.y + r.h - 1, r.w - 4, 1, outline);
    ui_fill(c, r.x, r.y + 2, 1, r.h - 4, outline);
    ui_fill(c, r.x + r.w - 1, r.y + 2, 1, r.h - 4, outline);
    ui_pixel(c, r.x + 1, r.y + 1, outline);
    ui_pixel(c, r.x + r.w - 2, r.y + 1, outline);
    ui_pixel(c, r.x + 1, r.y + r.h - 2, outline);
    ui_pixel(c, r.x + r.w - 2, r.y + r.h - 2, outline);
}
static void triangle_down(ui_canvas *c, int x, int y, int size, uint32_t color) {
    for (int i = 0; i < size; ++i) ui_fill(c, x + i, y + i, 2 * (size - i) - 1, 1, color);
}
static void triangle_up(ui_canvas *c, int x, int y, int size, uint32_t color) {
    for (int i = 0; i < size; ++i) ui_fill(c, x + size - 1 - i, y + i, 2 * i + 1, 1, color);
}
static void triangle_right(ui_canvas *c, int x, int y, int size, uint32_t color) {
    for (int i = 0; i < size; ++i) ui_fill(c, x + i, y + i, 1, 2 * (size - i) - 1, color);
}
static const ui_font *font(int which) { return ui_font_get(which); }
static void text_centered(ui *u, ui_rect r, const char *text, uint32_t color) {
    const ui_font *f = font(UI_FONT_SYSTEM);
    ui_draw_text(&u->canvas, f, r.x + (r.w - ui_text_width(f, text)) / 2, r.y + (r.h - ui_font_height(f)) / 2 + 1, text, color);
}
/* The focus ring: two pixels of the accent around r. */
static void focus_ring(ui *u, ui_rect r) {
    ui_frame(&u->canvas, r.x - 2, r.y - 2, r.w + 4, r.h + 4, UI_ACCENT_DARK);
    ui_frame(&u->canvas, r.x - 1, r.y - 1, r.w + 2, r.h + 2, UI_ACCENT_DARK);
}

/* --- frames and input ----------------------------------------------------------------- */

uint32_t ui_id(const char *name, int index) {
    uint32_t h = 2166136261u;
    for (const char *p = name; *p; ++p) h = (h ^ (uint8_t)*p) * 16777619u;
    h = (h ^ (uint32_t)index) * 16777619u;
    return h ? h : 1;
}
int ui_inside(ui_rect r, int x, int y) { return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h; }
void ui_register(ui *u, uint32_t id, ui_rect r, const char *label, int kind) {
    if (u->hit_count >= UI_HITS) return;
    u->hits[u->hit_count].r = r;
    u->hits[u->hit_count].id = id;
    u->hits[u->hit_count].kind = kind;
    snprintf(u->hits[u->hit_count].label, sizeof u->hits[0].label, "%s", label ? label : "");
    u->hit_count++;
}
static void focusable(ui *u, uint32_t id) {
    if (!u->blocked && u->order_count < 256) u->order[u->order_count++] = id;
}
void ui_focus(ui *u, uint32_t id) {
    if (u->focus != id) u->edit_id = 0;
    u->focus = id;
}

/* Whether this control takes the frame's press, and whether it was clicked
 * (pressed and released on it). */
static int pressed_on(ui *u, uint32_t id, ui_rect r) {
    if (u->blocked || !u->in.pressed || !ui_inside(r, u->in.x, u->in.y)) return 0;
    u->active = id;
    u->in.pressed = 0;         /* taken */
    return 1;
}
static int released_on(ui *u, uint32_t id, ui_rect r) {
    if (u->blocked || !u->in.released || u->active != id) return 0;
    u->active = 0;
    u->in.released = 0;
    return ui_inside(r, u->in.x, u->in.y);
}
static int key_for(ui *u, uint32_t id, int key) {
    if (u->blocked || u->focus != id || u->in.key != key) return 0;
    u->in.key = UI_KEY_NONE;
    return 1;
}

static void close_menu(ui *u) { u->menu.id = 0; }
/* The open menu's geometry: the scroll arrows' height (0 when every item
 * shows) and the top of its first row. */
static int menu_arrows(const ui *u) { return u->menu.visible < u->menu.count ? u->menu.item_height : 0; }
static int menu_rows_y(const ui *u) { return u->menu.at.y + 2 + menu_arrows(u); }
static void menu_scroll_to(ui *u, int first) {
    const int max = u->menu.count - u->menu.visible;
    u->menu.first = first < 0 ? 0 : first > max ? (max > 0 ? max : 0) : first;
}
/* Scrolls so that item index shows. */
static void menu_show(ui *u, int index) {
    if (index < u->menu.first) menu_scroll_to(u, index);
    else if (index >= u->menu.first + u->menu.visible) menu_scroll_to(u, index - u->menu.visible + 1);
}
int ui_menu_reveal(ui *u, const char *label) {
    if (!u->menu.id) return 0;
    for (int i = 0; i < u->menu.count; ++i)
        if (!strcmp(u->menu.items[i], label)) { menu_show(u, i); return 1; }
    return 0;
}
/* The next enabled item from h in steps of step (wrapping when wrap), else h. */
static int menu_step(const ui *u, int h, int step, int wrap) {
    for (int k = 0, at = h; k < u->menu.count; ++k) {
        at += step;
        if (wrap) at = (at + u->menu.count) % u->menu.count;
        else if (at < 0 || at >= u->menu.count) break;
        if (u->menu.enabled[at]) return at;
    }
    return h;
}
/* The open menu takes the mouse and the keys first. */
static void menu_input(ui *u) {
    if (!u->menu.id) return;
    const ui_rect at = u->menu.at;
    const int item = u->menu.item_height, arrows = menu_arrows(u), rows_y = menu_rows_y(u);
    const int inside = ui_inside(at, u->in.x, u->in.y);
    const int on_up = inside && arrows && u->in.y < rows_y;
    const int on_down = inside && arrows && u->in.y >= rows_y + u->menu.visible * item;
    const int moved = u->in.x != u->menu.mouse_x || u->in.y != u->menu.mouse_y;
    u->menu.mouse_x = u->in.x;
    u->menu.mouse_y = u->in.y;
    /* The wheel and the scroll arrows (the mouse resting on one scrolls a row every third frame). */
    if (u->in.wheel) {
        menu_scroll_to(u, u->menu.first - u->in.wheel * 3);
        u->in.wheel = 0;
    }
    if (on_up || on_down) {
        if (u->menu.scroll_tick++ % 3 == 0) menu_scroll_to(u, u->menu.first + (on_up ? -1 : 1));
        u->menu.hover = -1;
    } else {
        u->menu.scroll_tick = 0;
        /* The hover follows the mouse when it moves, so the keys move it otherwise. */
        if (inside && (moved || u->in.pressed || u->in.released)) {
            const int row = (u->in.y - rows_y) / item;
            u->menu.hover = u->in.y >= rows_y && row < u->menu.visible ? u->menu.first + row : -1;
        }
    }
    /* A click opened the menu: it stays open for the next click (a press
     * dragged to an item and released there chooses it at once). */
    if (u->menu.sticky && u->in.released) {
        u->menu.sticky = 0;
        const int dx = u->in.x - u->menu.open_x, dy = u->in.y - u->menu.open_y;
        if (!inside || (dx * dx + dy * dy < 16)) { u->in.released = 0; u->active = 0; return; }
    }
    if (u->in.pressed) u->menu.sticky = 0;
    if (u->in.key == UI_KEY_ESCAPE) { close_menu(u); u->in.key = 0; return; }
    const int from = u->menu.hover < 0 ? u->menu.current : u->menu.hover;
    const int last = u->menu.count - 1;
    int to = -2, back = 0;
    switch (u->in.key) {
    case UI_KEY_UP: to = menu_step(u, from, -1, 1); break;
    case UI_KEY_DOWN: to = menu_step(u, from, 1, 1); break;
    case UI_KEY_PAGE_UP: to = from - u->menu.visible + 1 > 0 ? from - u->menu.visible + 1 : 0; back = 1; break;
    case UI_KEY_PAGE_DOWN: to = from + u->menu.visible - 1 < last ? from + u->menu.visible - 1 : last; break;
    case UI_KEY_HOME: to = 0; break;
    case UI_KEY_END: to = last; back = 1; break;
    default: break;
    }
    if (to != -2) {
        /* A page or an end lands on an enabled item: the nearest onwards, else the other way. */
        if (to >= 0 && to <= last && !u->menu.enabled[to]) {
            const int next = menu_step(u, to, back ? -1 : 1, 0);
            to = next != to ? next : menu_step(u, to, back ? 1 : -1, 0);
        }
        if (to >= 0 && to <= last && u->menu.enabled[to]) { u->menu.hover = to; menu_show(u, to); }
        u->in.key = 0;
        return;
    }
    int choose = -1;
    if ((u->in.key == UI_KEY_ENTER || u->in.key == UI_KEY_SPACE) && u->menu.hover >= 0) { choose = u->menu.hover; u->in.key = 0; }
    if (u->in.released && inside && !on_up && !on_down && u->menu.hover >= 0 && u->menu.hover < u->menu.count) choose = u->menu.hover;
    if (choose >= 0 && choose < u->menu.count && u->menu.enabled[choose]) {
        u->menu.chosen_id = u->menu.id;
        u->menu.chosen = choose;
        close_menu(u);
    } else if (u->in.pressed && !inside) {
        close_menu(u);
    }
    /* The menu keeps the mouse from the controls under it. */
    if (inside || choose >= 0) { u->in.pressed = 0; u->in.released = 0; u->active = 0; }
    else if (u->in.pressed) u->in.pressed = 0;
}

void ui_begin(ui *u, const ui_input *in) {
    u->in = *in;
    if (in->pressed) u->down = 1;
    if (in->released) u->down = 0;
    u->hit_count = 0;
    memcpy(u->last_order, u->order, sizeof u->order);
    u->last_order_count = u->order_count;
    u->order_count = 0;
    u->blocked = 0;
    ui_clear_clip(&u->canvas);
    menu_input(u);
    /* Tab moves the focus along last frame's controls. */
    u->focus_moved = 0;
    if (u->in.key == UI_KEY_TAB && u->last_order_count) {
        int at = -1;
        for (int i = 0; i < u->last_order_count; ++i) if (u->last_order[i] == u->focus) at = i;
        const int step = u->in.mods & UI_MOD_SHIFT ? -1 : 1;
        at = at < 0 ? (step > 0 ? 0 : u->last_order_count - 1) : (at + step + u->last_order_count) % u->last_order_count;
        ui_focus(u, u->last_order[at]);
        u->focus_moved = 1;
        u->in.key = 0;
    }
    if (u->in.pressed) u->active = 0;
}

void ui_end(ui *u) {
    if (u->in.released) u->active = 0;
    if (!u->menu.id) return;
    /* The open menu over everything, with its items registered for scripts. */
    ui_canvas *c = &u->canvas;
    const ui_rect at = u->menu.at;
    ui_fill(c, at.x + 2, at.y + at.h, at.w, 2, UI_DARK);
    ui_fill(c, at.x + at.w, at.y + 2, 2, at.h, UI_DARK);
    ui_fill(c, at.x, at.y, at.w, at.h, UI_FACE);
    ui_frame(c, at.x, at.y, at.w, at.h, UI_BLACK);
    const ui_font *f = font(UI_FONT_SYSTEM);
    const int arrows = menu_arrows(u), rows_y = menu_rows_y(u);
    if (arrows) {
        /* The scroll arrows, dimmed at either end. */
        const int ax = at.x + at.w / 2 - 5;
        triangle_up(c, ax, at.y + 2 + arrows / 2 - 3, 6, u->menu.first > 0 ? UI_BLACK : UI_DISABLED);
        triangle_down(c, ax, rows_y + u->menu.visible * u->menu.item_height + arrows / 2 - 3, 6,
                      u->menu.first + u->menu.visible < u->menu.count ? UI_BLACK : UI_DISABLED);
    }
    for (int k = 0; k < u->menu.visible; ++k) {
        const int i = u->menu.first + k;
        if (i >= u->menu.count) break;
        const ui_rect row = {at.x + 1, rows_y + k * u->menu.item_height, at.w - 2, u->menu.item_height};
        const int hover = i == u->menu.hover && u->menu.enabled[i];
        if (hover) ui_fill(c, row.x, row.y, row.w, row.h, MENU_SELECTED);
        const uint32_t color = !u->menu.enabled[i] ? UI_DISABLED : hover ? UI_WHITE : UI_BLACK;
        if (i == u->menu.current) {
            /* The check mark of the current item. */
            const int x = row.x + 4, y = row.y + 4;
            for (int j = 0; j < 3; ++j) { ui_fill(c, x + j, y + 4 + j, 2, 1, color); }
            for (int j = 0; j < 6; ++j) { ui_fill(c, x + 3 + j, y + 6 - j, 2, 1, color); }
        }
        ui_draw_text(c, f, row.x + 16, row.y + (row.h - ui_font_height(f)) / 2 + 1, u->menu.items[i], color);
        ui_register(u, ui_id("menu-item", i), row, u->menu.items[i], UI_HIT_MENU_ITEM);
    }
}

/* --- windows and frames ------------------------------------------------------------- */

static void stripes(ui_canvas *c, int x, int y, int w, int h) {
    for (int j = 0; j < h; ++j) {
        if (j % 2 == 0) ui_fill(c, x, y + j, w, 1, UI_WHITE);
        else ui_fill(c, x, y + j, w, 1, UI_SHADOW);
    }
}
ui_rect ui_window(ui *u, ui_rect r, const char *title) {
    ui_canvas *c = &u->canvas;
    ui_fill(c, r.x, r.y, r.w, r.h, UI_FACE);
    ui_frame(c, r.x, r.y, r.w, r.h, UI_BLACK);
    ui_bevel(c, (ui_rect){r.x + 1, r.y + 1, r.w - 2, r.h - 2}, 0);
    /* The title bar: pinstripes, the close box at the left, the title. */
    const int bar = 19;
    ui_fill(c, r.x + 2, r.y + 2, r.w - 4, bar, UI_FACE);
    stripes(c, r.x + 6, r.y + 5, r.w - 12, 11);
    const ui_font *f = font(UI_FONT_SYSTEM);
    const int tw = ui_text_width(f, title), tx = r.x + (r.w - tw) / 2;
    ui_fill(c, tx - 8, r.y + 3, tw + 16, bar - 2, UI_FACE);
    ui_draw_text(c, f, tx, r.y + 5, title, UI_BLACK);
    const ui_rect box = {r.x + 9, r.y + 5, 11, 11};
    ui_fill(c, box.x - 2, box.y - 1, box.w + 4, box.h + 2, UI_FACE);
    ui_frame(c, box.x, box.y, box.w, box.h, UI_BLACK);
    ui_bevel(c, (ui_rect){box.x + 1, box.y + 1, box.w - 2, box.h - 2}, 0);
    ui_fill(c, r.x + 2, r.y + bar + 2, r.w - 4, 1, UI_SHADOW);
    return (ui_rect){r.x + 3, r.y + bar + 4, r.w - 6, r.h - bar - 7};
}
ui_rect ui_dialog(ui *u, ui_rect r) {
    ui_canvas *c = &u->canvas;
    ui_fill(c, r.x + 3, r.y + r.h, r.w, 3, UI_SHADOW);
    ui_fill(c, r.x + r.w, r.y + 3, 3, r.h, UI_SHADOW);
    ui_fill(c, r.x, r.y, r.w, r.h, UI_FACE);
    ui_frame(c, r.x, r.y, r.w, r.h, UI_BLACK);
    ui_bevel(c, (ui_rect){r.x + 1, r.y + 1, r.w - 2, r.h - 2}, 0);
    ui_frame(c, r.x + 3, r.y + 3, r.w - 6, r.h - 6, UI_SHADOW);
    ui_bevel(c, (ui_rect){r.x + 4, r.y + 4, r.w - 8, r.h - 8}, 0);
    return (ui_rect){r.x + 12, r.y + 12, r.w - 24, r.h - 24};
}
ui_rect ui_group(ui *u, ui_rect r, const char *title) {
    ui_canvas *c = &u->canvas;
    const ui_font *f = font(UI_FONT_SYSTEM);
    const int top = r.y + ui_font_height(f) / 2;
    ui_frame(c, r.x, top, r.w - 1, r.y + r.h - top - 1, UI_SHADOW);
    ui_frame(c, r.x + 1, top + 1, r.w - 1, r.y + r.h - top - 1, UI_WHITE);
    if (title && *title) {
        const int w = ui_text_width(f, title);
        ui_fill(c, r.x + 8, r.y, w + 6, ui_font_height(f), UI_FACE);
        ui_draw_text(c, f, r.x + 11, r.y, title, UI_BLACK);
    }
    return (ui_rect){r.x + 10, r.y + ui_font_height(f) + 6, r.w - 20, r.h - ui_font_height(f) - 12};
}
void ui_label(ui *u, int x, int y, const char *text, int which, uint32_t color) {
    ui_draw_text(&u->canvas, font(which), x, y, text, color);
}
int ui_wrapped(ui *u, ui_rect r, const char *text, int which, uint32_t color) {
    const ui_font *f = font(which);
    const int line = ui_font_height(f) + 2;
    int y = r.y;
    const char *s = text;
    while (*s) {
        /* The longest run of words that fits. */
        const char *end = s, *fit = NULL;
        while (1) {
            const char *next = end;
            while (*next == ' ') next++;
            while (*next && *next != ' ' && *next != '\n') next++;
            if (fit && ui_text_width_n(f, s, (size_t)(next - s)) > r.w) break;
            fit = next;
            end = next;
            if (!*next || *next == '\n') break;
        }
        char piece[512];
        snprintf(piece, sizeof piece, "%.*s", (int)(fit - s), s);
        if (y + line <= r.y + r.h || r.h <= 0) ui_draw_text(&u->canvas, f, r.x, y, piece, color);
        y += line;
        s = fit;
        while (*s == ' ') s++;
        if (*s == '\n') s++;
    }
    return y - r.y;
}

/* --- controls ----------------------------------------------------------------------- */

int ui_button(ui *u, uint32_t id, ui_rect r, const char *label, int flags) {
    ui_canvas *c = &u->canvas;
    const int disabled = flags & UI_DISABLED_FLAG;
    ui_register(u, id, r, label, UI_HIT_BUTTON);
    int clicked = 0;
    if (!disabled) {
        focusable(u, id);
        if (pressed_on(u, id, r)) ui_focus(u, id);
        clicked = released_on(u, id, r) || key_for(u, id, UI_KEY_SPACE) || key_for(u, id, UI_KEY_ENTER);
    }
    const int held = !disabled && u->active == id && u->down && ui_inside(r, u->in.x, u->in.y);
    if (flags & UI_DEFAULT) {
        /* The default button's ring. */
        const ui_rect ring = {r.x - 3, r.y - 3, r.w + 6, r.h + 6};
        rounded(c, ring, disabled ? UI_DISABLED : UI_BLACK, UI_SHADE);
        ui_fill(c, ring.x + 2, ring.y + 1, ring.w - 4, 1, UI_WHITE);
        ui_fill(c, ring.x + 1, ring.y + 2, 1, ring.h - 4, UI_WHITE);
    }
    rounded(c, r, disabled ? UI_DISABLED : UI_BLACK, held ? UI_SHADOW : UI_FACE);
    if (!held) {
        ui_fill(c, r.x + 2, r.y + 1, r.w - 4, 1, UI_WHITE);
        ui_fill(c, r.x + 1, r.y + 2, 1, r.h - 4, UI_WHITE);
        ui_fill(c, r.x + 2, r.y + r.h - 2, r.w - 4, 1, UI_SHADE);
        ui_fill(c, r.x + r.w - 2, r.y + 2, 1, r.h - 4, UI_SHADE);
    }
    text_centered(u, r, label, disabled ? UI_DISABLED : held ? UI_WHITE : UI_BLACK);
    if (u->focus == id && !disabled && !(flags & UI_DEFAULT)) focus_ring(u, (ui_rect){r.x - 1, r.y - 1, r.w + 2, r.h + 2});
    return clicked;
}

int ui_checkbox(ui *u, uint32_t id, int x, int y, const char *label, int *value, int disabled) {
    ui_canvas *c = &u->canvas;
    const ui_font *f = font(UI_FONT_SYSTEM);
    const ui_rect box = {x, y + 1, 12, 12}, hit = {x, y, 16 + ui_text_width(f, label), 14};
    ui_register(u, id, hit, label, UI_HIT_CHECKBOX);
    int changed = 0;
    if (!disabled) {
        focusable(u, id);
        if (pressed_on(u, id, hit)) ui_focus(u, id);
        if (released_on(u, id, hit) || key_for(u, id, UI_KEY_SPACE)) { *value = !*value; changed = 1; }
    }
    const int held = !disabled && u->active == id && u->down;
    ui_fill(c, box.x, box.y, box.w, box.h, held ? UI_SHADE : UI_WHITE);
    ui_frame(c, box.x, box.y, box.w, box.h, disabled ? UI_DISABLED : UI_BLACK);
    ui_fill(c, box.x + 1, box.y + 1, box.w - 2, 1, UI_SHADE);
    ui_fill(c, box.x + 1, box.y + 1, 1, box.h - 2, UI_SHADE);
    if (*value) {
        const uint32_t ink = disabled ? UI_DISABLED : UI_BLACK;
        for (int k = 0; k < 3; ++k) ui_fill(c, box.x + 2 + k, box.y + 5 + k, 2, 2, ink);
        for (int k = 0; k < 6; ++k) ui_fill(c, box.x + 5 + k, box.y + 7 - k, 2, 2, ink);
    }
    ui_draw_text(c, f, x + 17, y + 1, label, disabled ? UI_DISABLED : UI_BLACK);
    if (u->focus == id && !disabled) focus_ring(u, (ui_rect){box.x - 1, box.y - 1, box.w + 2, box.h + 2});
    return changed;
}

int ui_radio(ui *u, uint32_t id, int x, int y, const char *label, int selected, int disabled) {
    ui_canvas *c = &u->canvas;
    const ui_font *f = font(UI_FONT_SYSTEM);
    const ui_rect hit = {x, y, 16 + ui_text_width(f, label), 14};
    ui_register(u, id, hit, label, UI_HIT_RADIO);
    int clicked = 0;
    if (!disabled) {
        focusable(u, id);
        if (pressed_on(u, id, hit)) ui_focus(u, id);
        clicked = released_on(u, id, hit) || key_for(u, id, UI_KEY_SPACE);
    }
    /* A 12-pixel circle. */
    static const char *const ring[12] = {"....####....", "..##....##..", ".#........#.", ".#........#.", "#..........#",
                                         "#..........#", "#..........#", "#..........#", ".#........#.", ".#........#.",
                                         "..##....##..", "....####...."};
    const uint32_t ink = disabled ? UI_DISABLED : UI_BLACK;
    for (int j = 0; j < 12; ++j) {
        int first = -1, last = -1;
        for (int i = 0; i < 12; ++i) if (ring[j][i] == '#') { if (first < 0) first = i; last = i; }
        if (first >= 0 && last > first + 1) ui_fill(c, x + first + 1, y + 1 + j, last - first - 1, 1, UI_WHITE);
        for (int i = 0; i < 12; ++i) if (ring[j][i] == '#') ui_pixel(c, x + i, y + 1 + j, ink);
    }
    if (selected) ui_fill(c, x + 4, y + 5, 4, 4, ink), ui_fill(c, x + 5, y + 4, 2, 6, ink), ui_fill(c, x + 3, y + 6, 6, 2, ink);
    ui_draw_text(c, f, x + 17, y + 1, label, disabled ? UI_DISABLED : UI_BLACK);
    if (u->focus == id && !disabled) focus_ring(u, (ui_rect){x - 1, y, 14, 14});
    return clicked;
}

int ui_popup(ui *u, uint32_t id, ui_rect r, const char *name, const char *const *items, int count, int *selected,
             const uint8_t *enabled, const char *text_when_none) {
    ui_canvas *c = &u->canvas;
    const int disabled = count == 0;
    ui_register(u, id, r, name, UI_HIT_POPUP);
    int changed = 0;
    if (u->menu.chosen_id == id) {
        u->menu.chosen_id = 0;
        if (u->menu.chosen != *selected) { *selected = u->menu.chosen; changed = 1; }
    }
    int open = 0;
    if (!disabled) {
        focusable(u, id);
        if (pressed_on(u, id, r)) { ui_focus(u, id); open = 1; }
        if (key_for(u, id, UI_KEY_SPACE)) open = 1;
        /* Up and Down step through the enabled items without opening. */
        for (int dir = -1; dir <= 1; dir += 2) {
            if (!key_for(u, id, dir < 0 ? UI_KEY_UP : UI_KEY_DOWN)) continue;
            for (int k = *selected + dir; k >= 0 && k < count; k += dir)
                if (!enabled || enabled[k]) { *selected = k; changed = 1; break; }
        }
    }
    if (open && count) {
        const ui_font *f = font(UI_FONT_SYSTEM);
        int w = r.w;
        for (int i = 0; i < count && i < UI_MENU_ITEMS; ++i) {
            const int need = ui_text_width(f, items[i]) + 30;
            if (need > w) w = need;
        }
        const int n = count < UI_MENU_ITEMS ? count : UI_MENU_ITEMS, item = ui_font_height(f) + 4;
        /* A menu taller than the canvas shows what fits, with scroll arrows. */
        const int fit = (c->h - 8) / item;
        const int visible = n <= fit ? n : fit - 2 > 1 ? fit - 2 : 1;
        const int sel = *selected > 0 && *selected < n ? *selected : 0;
        u->menu.count = n;
        u->menu.visible = visible;
        u->menu.item_height = item;
        u->menu.first = 0;
        menu_scroll_to(u, sel - visible / 2);
        const int arrows = menu_arrows(u);
        const int h = visible * item + 4 + 2 * arrows;
        int y = r.y - (sel - u->menu.first) * item - 2 - arrows;
        if (y + h > c->h - 2) y = c->h - 2 - h;
        if (y < 2) y = 2;
        int x = r.x;
        if (x + w > c->w - 2) x = c->w - 2 - w;
        u->menu.id = id;
        u->menu.at = (ui_rect){x, y, w, h};
        u->menu.current = *selected;
        u->menu.hover = *selected;
        u->menu.scroll_tick = 0;
        u->menu.mouse_x = u->in.x;
        u->menu.mouse_y = u->in.y;
        u->menu.sticky = 1;
        u->menu.open_x = u->in.x;
        u->menu.open_y = u->in.y;
        for (int i = 0; i < n; ++i) {
            snprintf(u->menu.items[i], sizeof u->menu.items[i], "%s", items[i]);
            u->menu.enabled[i] = !enabled || enabled[i];
        }
    }
    rounded(c, r, disabled ? UI_DISABLED : UI_BLACK, UI_FACE);
    ui_fill(c, r.x + 2, r.y + 1, r.w - 4, 1, UI_WHITE);
    ui_fill(c, r.x + 1, r.y + 2, 1, r.h - 4, UI_WHITE);
    /* The arrow segment. */
    const int seg = 18;
    ui_fill(c, r.x + r.w - seg, r.y + 1, 1, r.h - 2, disabled ? UI_DISABLED : UI_BLACK);
    ui_fill(c, r.x + r.w - seg + 1, r.y + 1, seg - 2, r.h - 2, u->menu.id == id ? UI_SHADOW : UI_SHADE);
    triangle_down(c, r.x + r.w - seg + 4, r.y + r.h / 2 - 2, 5, disabled ? UI_DISABLED : UI_BLACK);
    const char *text = *selected >= 0 && *selected < count ? items[*selected] : text_when_none ? text_when_none : "";
    const ui_font *f = font(UI_FONT_SYSTEM);
    ui_set_clip(c, (ui_rect){r.x + 1, r.y, r.w - seg - 2, r.h});
    ui_draw_text(c, f, r.x + 7, r.y + (r.h - ui_font_height(f)) / 2 + 1, text, disabled ? UI_DISABLED : UI_BLACK);
    ui_clear_clip(c);
    if (u->focus == id && !disabled) focus_ring(u, r);
    return changed;
}

int ui_tabs(ui *u, uint32_t id, ui_rect r, const char *const *labels, int count, int *current, ui_rect *panel) {
    ui_canvas *c = &u->canvas;
    const ui_font *f = font(UI_FONT_SYSTEM);
    const int tab_h = 22;
    int changed = 0, x = r.x + 8;
    focusable(u, id);
    if (key_for(u, id, UI_KEY_LEFT) && *current > 0) { (*current)--; changed = 1; }
    if (key_for(u, id, UI_KEY_RIGHT) && *current + 1 < count) { (*current)++; changed = 1; }
    /* The panel. */
    const ui_rect p = {r.x, r.y + tab_h - 1, r.w, r.h - tab_h + 1};
    ui_fill(c, p.x, p.y, p.w, p.h, UI_FACE);
    ui_frame(c, p.x, p.y, p.w, p.h, UI_SHADOW);
    ui_bevel(c, (ui_rect){p.x + 1, p.y + 1, p.w - 2, p.h - 2}, 0);
    for (int i = 0; i < count; ++i) {
        const int w = ui_text_width(f, labels[i]) + 24;
        const ui_rect t = {x, r.y + (i == *current ? 0 : 2), w, tab_h - (i == *current ? 0 : 2)};
        const uint32_t tid = id ^ (uint32_t)(i + 1) * 2654435761u;
        ui_register(u, tid, t, labels[i], UI_HIT_TAB);
        if (pressed_on(u, tid, t)) { ui_focus(u, id); if (*current != i) { *current = i; changed = 1; } }
        const uint32_t face = i == *current ? UI_FACE : UI_SHADE;
        ui_fill(c, t.x + 1, t.y + 1, t.w - 2, t.h - 1, face);
        ui_fill(c, t.x + 2, t.y, t.w - 4, 1, UI_SHADOW);
        ui_fill(c, t.x, t.y + 2, 1, t.h - 2, UI_SHADOW);
        ui_fill(c, t.x + t.w - 1, t.y + 2, 1, t.h - 2, UI_SHADOW);
        ui_pixel(c, t.x + 1, t.y + 1, UI_SHADOW);
        ui_pixel(c, t.x + t.w - 2, t.y + 1, UI_SHADOW);
        ui_fill(c, t.x + 2, t.y + 1, t.w - 4, 1, UI_WHITE);
        ui_fill(c, t.x + 1, t.y + 2, 1, t.h - 3, UI_WHITE);
        if (i == *current) ui_fill(c, t.x + 1, p.y, t.w - 2, 2, face);
        ui_draw_text(c, f, t.x + 12, t.y + 5, labels[i], UI_BLACK);
        if (i == *current && u->focus == id) ui_frame(c, t.x + 6, t.y + 3, t.w - 12, t.h - 6, UI_ACCENT_DARK);
        x += w - 1;
    }
    if (panel) *panel = (ui_rect){p.x + 10, p.y + 10, p.w - 20, p.h - 20};
    return changed;
}

/* A Platinum scroll bar; returns 1 when the position moved. */
static int scroll_bar(ui *u, uint32_t id, ui_rect r, int total, int visible, int *pos) {
    ui_canvas *c = &u->canvas;
    const int max = total > visible ? total - visible : 0;
    int changed = 0;
    const ui_rect up = {r.x, r.y, r.w, r.w}, down = {r.x, r.y + r.h - r.w, r.w, r.w};
    const ui_rect track = {r.x, r.y + r.w, r.w, r.h - 2 * r.w};
    if (max) {
        if (pressed_on(u, id + 1, up) && *pos > 0) { (*pos)--; changed = 1; }
        if (pressed_on(u, id + 2, down) && *pos < max) { (*pos)++; changed = 1; }
    }
    ui_fill(c, r.x, r.y, r.w, r.h, max ? UI_SHADE : UI_FACE);
    ui_frame(c, r.x, r.y, r.w, r.h, UI_BLACK);
    for (int k = 0; k < 2; ++k) {
        const ui_rect b = k ? down : up;
        ui_fill(c, b.x + 1, b.y + 1, b.w - 2, b.h - 2, UI_FACE);
        ui_frame(c, b.x, b.y, b.w, b.h, UI_BLACK);
        ui_bevel(c, (ui_rect){b.x + 1, b.y + 1, b.w - 2, b.h - 2}, 0);
        if (k) triangle_down(c, b.x + b.w / 2 - 3, b.y + b.h / 2 - 1, 4, max ? UI_BLACK : UI_DISABLED);
        else triangle_up(c, b.x + b.w / 2 - 3, b.y + b.h / 2 - 2, 4, max ? UI_BLACK : UI_DISABLED);
    }
    if (max && track.h > 16) {
        int thumb = track.h * visible / total;
        if (thumb < 16) thumb = 16;
        const int span = track.h - thumb;
        int at = track.y + span * *pos / max;
        const ui_rect t = {track.x, at, track.w, thumb};
        if (pressed_on(u, id + 3, t)) u->drag_offset = u->in.y - at;
        else if (pressed_on(u, id + 4, track)) {
            *pos += u->in.y < at ? -visible : visible;
            if (*pos < 0) *pos = 0;
            if (*pos > max) *pos = max;
            changed = 1;
        }
        if (u->active == id + 3 && u->down && span > 0) {
            int p = (u->in.y - u->drag_offset - track.y) * max / span;
            if (p < 0) p = 0;
            if (p > max) p = max;
            if (p != *pos) { *pos = p; changed = 1; }
            at = track.y + span * *pos / max;
        }
        const ui_rect th = {track.x, at, track.w, thumb};
        ui_fill(c, th.x + 1, th.y, th.w - 2, th.h, UI_FACE);
        ui_frame(c, th.x, th.y, th.w, th.h, UI_BLACK);
        ui_bevel(c, (ui_rect){th.x + 1, th.y + 1, th.w - 2, th.h - 2}, 0);
        for (int g = -2; g <= 2; g += 2) ui_fill(c, th.x + 4, th.y + th.h / 2 + g, th.w - 8, 1, UI_SHADOW);
    }
    return changed;
}

int ui_list(ui *u, uint32_t id, ui_rect r, int row_height, int count, int *selected, int *scroll, ui_row_fn draw_row, void *ctx,
            const char *const *row_labels, int *activated) {
    ui_canvas *c = &u->canvas;
    const int bar = 16;
    const ui_rect inner = {r.x + 1, r.y + 1, r.w - bar - 1, r.h - 2};
    const int visible = inner.h / row_height;
    int changed = 0;
    if (activated) *activated = 0;
    u->list_clicked = -1;
    focusable(u, id);
    if (*scroll > count - visible) *scroll = count - visible;
    if (*scroll < 0) *scroll = 0;
    /* Keys move the selection. */
    if (count) {
        int to = *selected;
        if (key_for(u, id, UI_KEY_UP)) to = to > 0 ? to - 1 : 0;
        else if (key_for(u, id, UI_KEY_DOWN)) to = to + 1 < count ? to + 1 : count - 1;
        else if (key_for(u, id, UI_KEY_HOME)) to = 0;
        else if (key_for(u, id, UI_KEY_END)) to = count - 1;
        else if (key_for(u, id, UI_KEY_ENTER) && activated) *activated = 1;
        if (to != *selected && to >= 0) { *selected = to; changed = 1; }
        if (changed) {
            if (*selected < *scroll) *scroll = *selected;
            if (*selected >= *scroll + visible) *scroll = *selected - visible + 1;
        }
    }
    if (!u->blocked && u->in.wheel && ui_inside(r, u->in.x, u->in.y)) {
        *scroll -= u->in.wheel;
        if (*scroll > count - visible) *scroll = count - visible;
        if (*scroll < 0) *scroll = 0;
    }
    ui_fill(c, r.x, r.y, r.w, r.h, UI_WHITE);
    ui_frame(c, r.x, r.y, r.w, r.h, UI_BLACK);
    for (int i = 0; i < visible + 1 && *scroll + i < count; ++i) {
        const int index = *scroll + i;
        const ui_rect row = {inner.x, inner.y + i * row_height, inner.w, row_height};
        if (row.y >= inner.y + inner.h) break;
        ui_register(u, id ^ (uint32_t)(index + 1) * 40503u, row, row_labels ? row_labels[index] : NULL, UI_HIT_ROW);
        if (pressed_on(u, id, row)) {
            ui_focus(u, id);
            u->list_clicked = index;
            if (*selected != index) { *selected = index; changed = 1; }
            if (u->in.double_click && activated) *activated = 1;
        }
        ui_set_clip(c, (ui_rect){inner.x, inner.y, inner.w, inner.h});
        if (index == *selected) ui_fill(c, row.x, row.y, row.w, row.h, UI_ACCENT);
        draw_row(ctx, u, row, index, index == *selected);
        ui_clear_clip(c);
    }
    scroll_bar(u, id + 16, (ui_rect){r.x + r.w - bar, r.y, bar, r.h}, count, visible, scroll);
    if (u->focus == id) focus_ring(u, r);
    return changed;
}

/* --- edit fields --------------------------------------------------------------------- */

static size_t utf8_back(const char *s, size_t at) {
    while (at > 0) { at--; if (((unsigned char)s[at] & 0xC0) != 0x80) break; }
    return at;
}
static size_t utf8_forward(const char *s, size_t at) {
    const size_t n = strlen(s);
    if (at < n) at++;
    while (at < n && ((unsigned char)s[at] & 0xC0) == 0x80) at++;
    return at;
}
void ui_field(ui *u, ui_rect r, const char *text) {
    ui_canvas *c = &u->canvas;
    ui_fill(c, r.x, r.y, r.w, r.h, UI_FACE);
    ui_bevel(c, r, 1);
    ui_frame(c, r.x + 1, r.y + 1, r.w - 2, r.h - 2, UI_SHADOW);
    const ui_font *f = font(UI_FONT_SYSTEM);
    ui_set_clip(c, (ui_rect){r.x + 2, r.y + 2, r.w - 4, r.h - 4});
    ui_draw_text(c, f, r.x + 5, r.y + (r.h - ui_font_height(f)) / 2 + 1, text, UI_BLACK);
    ui_clear_clip(c);
}
int ui_edit(ui *u, uint32_t id, ui_rect r, char *text, size_t capacity, int disabled) {
    ui_canvas *c = &u->canvas;
    const ui_font *f = font(UI_FONT_SYSTEM);
    ui_register(u, id, r, NULL, UI_HIT_EDIT);
    int changed = 0;
    if (!disabled) {
        focusable(u, id);
        if (pressed_on(u, id, r)) {
            ui_focus(u, id);
            u->edit_id = id;
            /* The caret at the click. */
            size_t at = 0;
            while (text[at] && r.x + 5 + ui_text_width_n(f, text, utf8_forward(text, at)) <= u->in.x) at = utf8_forward(text, at);
            u->caret = (int)at;
        }
        if (u->focus == id && u->edit_id != id) { u->edit_id = id; u->caret = (int)strlen(text); }
        if (u->focus == id && !u->blocked) {
            size_t n = strlen(text), at = (size_t)u->caret > n ? n : (size_t)u->caret;
            switch (u->in.key) {
            case UI_KEY_LEFT: at = utf8_back(text, at); u->in.key = 0; break;
            case UI_KEY_RIGHT: at = utf8_forward(text, at); u->in.key = 0; break;
            case UI_KEY_HOME: at = 0; u->in.key = 0; break;
            case UI_KEY_END: at = n; u->in.key = 0; break;
            case UI_KEY_BACKSPACE:
                if (at) { const size_t from = utf8_back(text, at); memmove(text + from, text + at, n - at + 1); at = from; changed = 1; }
                u->in.key = 0;
                break;
            case UI_KEY_DELETE:
                if (at < n) { const size_t to = utf8_forward(text, at); memmove(text + at, text + to, n - to + 1); changed = 1; }
                u->in.key = 0;
                break;
            default: break;
            }
            const size_t add = strlen(u->in.text);
            if (add && !(u->in.mods & UI_MOD_CTRL)) {
                n = strlen(text);
                if (n + add < capacity) {
                    memmove(text + at + add, text + at, n - at + 1);
                    memcpy(text + at, u->in.text, add);
                    at += add;
                    changed = 1;
                }
                u->in.text[0] = 0;
            }
            u->caret = (int)at;
        }
    }
    ui_fill(c, r.x, r.y, r.w, r.h, disabled ? UI_FACE : UI_WHITE);
    ui_bevel(c, r, 1);
    ui_frame(c, r.x + 1, r.y + 1, r.w - 2, r.h - 2, disabled ? UI_SHADOW : UI_BLACK);
    /* The text scrolled so the caret shows. */
    const int room = r.w - 10;
    int shift = 0;
    if (u->focus == id) {
        const int caret_x = ui_text_width_n(f, text, (size_t)u->caret);
        if (caret_x > room) shift = caret_x - room;
    }
    ui_set_clip(c, (ui_rect){r.x + 2, r.y + 2, r.w - 4, r.h - 4});
    const int ty = r.y + (r.h - ui_font_height(f)) / 2 + 1;
    ui_draw_text(c, f, r.x + 5 - shift, ty, text, disabled ? UI_DISABLED : UI_BLACK);
    if (u->focus == id && !disabled) {
        const int cx = r.x + 5 - shift + ui_text_width_n(f, text, (size_t)u->caret);
        ui_fill(c, cx, ty - 1, 1, ui_font_height(f), UI_BLACK);
    }
    ui_clear_clip(c);
    if (u->focus == id && !disabled) focus_ring(u, r);
    return changed;
}

int ui_slider(ui *u, uint32_t id, ui_rect r, double *value, double mark, int disabled) {
    ui_canvas *c = &u->canvas;
    ui_register(u, id, r, NULL, UI_HIT_SLIDER);
    const int thumb_w = 11, x0 = r.x + thumb_w / 2, span = r.w - thumb_w;
    int changed = 0;
    if (!disabled && span > 0) {
        focusable(u, id);
        if (pressed_on(u, id, r)) ui_focus(u, id);
        if (u->active == id && u->down) {
            double v = (double)(u->in.x - x0) / span;
            v = v < 0 ? 0 : v > 1 ? 1 : v;
            if (v != *value) { *value = v; changed = 1; }
        }
        if (key_for(u, id, UI_KEY_LEFT)) { *value = *value > 0.01 ? *value - 0.01 : 0; changed = 1; }
        if (key_for(u, id, UI_KEY_RIGHT)) { *value = *value < 0.99 ? *value + 0.01 : 1; changed = 1; }
    }
    /* The channel, filled up to the thumb. */
    const int cy = r.y + 6, at = x0 + (int)(*value * span + 0.5);
    ui_fill(c, x0, cy, span, 5, UI_WHITE);
    ui_bevel(c, (ui_rect){x0 - 1, cy - 1, span + 2, 7}, 1);
    ui_fill(c, x0, cy, at - x0, 5, disabled ? UI_SHADE : UI_ACCENT_DARK);
    if (mark >= 0) {
        const int mx = x0 + (int)(mark * span + 0.5);
        ui_fill(c, mx, r.y + 15, 1, 4, UI_BLACK);
        triangle_up(c, mx - 2, r.y + 19, 3, UI_BLACK);
    }
    /* The thumb: a bevelled block pointing down. */
    const ui_rect t = {at - thumb_w / 2, r.y, thumb_w, 13};
    ui_fill(c, t.x + 1, t.y + 1, t.w - 2, t.h - 2, disabled ? UI_SHADE : UI_FACE);
    ui_frame(c, t.x, t.y, t.w, t.h, disabled ? UI_DISABLED : UI_BLACK);
    ui_bevel(c, (ui_rect){t.x + 1, t.y + 1, t.w - 2, t.h - 2}, 0);
    for (int g = 0; g < 3; ++g) ui_fill(c, t.x + 3 + 2 * g, t.y + 4, 1, 5, UI_SHADOW);
    if (u->focus == id && !disabled) focus_ring(u, t);
    return changed;
}

int ui_disclosure(ui *u, uint32_t id, int x, int y, const char *label, int *open) {
    ui_canvas *c = &u->canvas;
    const ui_font *f = font(UI_FONT_SYSTEM);
    const ui_rect hit = {x, y, 16 + ui_text_width(f, label), 14};
    ui_register(u, id, hit, label, UI_HIT_DISCLOSURE);
    focusable(u, id);
    int changed = 0;
    if (pressed_on(u, id, hit)) ui_focus(u, id);
    if (released_on(u, id, hit) || key_for(u, id, UI_KEY_SPACE)) { *open = !*open; changed = 1; }
    if (key_for(u, id, UI_KEY_RIGHT) && !*open) { *open = 1; changed = 1; }
    if (key_for(u, id, UI_KEY_LEFT) && *open) { *open = 0; changed = 1; }
    if (*open) {
        triangle_down(c, x + 1, y + 4, 6, UI_SHADOW);
        triangle_down(c, x + 2, y + 4, 5, UI_BLACK);
        triangle_down(c, x + 3, y + 4, 4, UI_SHADE);
    } else {
        triangle_right(c, x + 3, y + 1, 6, UI_BLACK);
        triangle_right(c, x + 4, y + 3, 4, UI_SHADE);
    }
    ui_draw_text(c, f, x + 17, y + 1, label, UI_BLACK);
    if (u->focus == id) focus_ring(u, (ui_rect){x, y + 1, 13, 12});
    return changed;
}
