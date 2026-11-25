/* The configuration window's icons, Livadix's own artwork: a hard disk, a floppy disk, a
 * CD, a compact Macintosh, a folder and a document (16 x 16, one character
 * per pixel: K black, W white, L light gray, g gray, G mid gray, D dark
 * gray, B screen blue, O green, P and p folder lavender, '.' clear), and the compatibility badges and alert icons, drawn
 * from shapes. */
#include "ui.h"

static const char *const hard_disk[16] = {
    "................", "................", "................", "................",
    ".KKKKKKKKKKKKKK.", "KWWWWWWWWWWWWWgK", "KWLLLLLLLLLLLLGK", "KWLLLLLLLLLLLLGK",
    "KWLLLLLLLLLOOLGK", "KWLLLLLLLLLLLLGK", "KgGGGGGGGGGGGGGK", ".KKKKKKKKKKKKKK.",
    "................", "................", "................", "................",
};
static const char *const floppy[16] = {
    "KKKKKKKKKKKKKKK.", "KDDDKWWWWWWKDDDK", "KDDDKWWWKKWKDDDK", "KDDDKWWWKKWKDDDK",
    "KDDDKWWWWWWKDDDK", "KDDDDKKKKKKDDDDK", "KDDDDDDDDDDDDDDK", "KDDWWWWWWWWWWDDK",
    "KDDWWWWWWWWWWDDK", "KDDWWKKKKKKWWDDK", "KDDWWWWWWWWWWDDK", "KDDWWKKKKKKWWDDK",
    "KDDWWWWWWWWWWDDK", "KDDWWWWWWWWWWDDK", "KDDWWWWWWWWWWDDK", "KKKKKKKKKKKKKKKK",
};
static const char *const cd[16] = {
    ".....KKKKKK.....", "...KKLLLLLLKK...", "..KLLLLWWLLLLK..", ".KLLLLWWWLLLLLK.",
    ".KLLLWWWLLLLLLK.", "KLLLLLLKKLLLLLLK", "KLLLLLKWWKLLLLLK", "KLLLLKW..WKLLLLK",
    "KLLLLKW..WKLLLLK", "KLLLLLKWWKLLLLLK", "KLLLLLLKKLLLLLLK", ".KLLLLLLLLggLLK.",
    ".KLLLLLLLggLLLK.", "..KLLLLLLLLLLK..", "...KKLLLLLLKK...", ".....KKKKKK.....",
};
static const char *const mac[16] = {
    "..KKKKKKKKKKKK..", "..KLLLLLLLLLLK..", "..KLKKKKKKKKLK..", "..KLKBBBBBBKLK..",
    "..KLKBWBBBBKLK..", "..KLKBBBBBBKLK..", "..KLKBBBBBBKLK..", "..KLKKKKKKKKLK..",
    "..KLLLLLLLLLLK..", "..KLLLLLLKKKLK..", "..KLLLLLLLLLLK..", "..KGGGGGGGGGGK..",
    "..KKKKKKKKKKKK..", "...KGGGGGGGGK...", "...KKKKKKKKKK...", "................",
};

static const char *const folder[16] = {
    "................", "................", "..KKKKK.........", ".KPPPPPK........",
    "KKKKKKKKKKKKKKK.", "KWWWWWWWWWWWWWpK", "KWPPPPPPPPPPPPpK", "KWPPPPPPPPPPPPpK",
    "KWPPPPPPPPPPPPpK", "KWPPPPPPPPPPPPpK", "KWPPPPPPPPPPPPpK", "KWPPPPPPPPPPPPpK",
    "KppppppppppppppK", "KKKKKKKKKKKKKKKK", "................", "................",
};
static const char *const document[16] = {
    "..KKKKKKKKK.....", "..KWWWWWWWKK....", "..KWWWWWWWKWK...", "..KWWWWWWWKWWK..",
    "..KWWWWWWWKKKKK.", "..KWWWWWWWWWWWK.", "..KWWWWWWWWWWWK.", "..KWWWWWWWWWWWK.",
    "..KWWWWWWWWWWWK.", "..KWWWWWWWWWWWK.", "..KWWWWWWWWWWWK.", "..KWWWWWWWWWWWK.",
    "..KWWWWWWWWWWWK.", "..KWWWWWWWWWWWK.", "..KWWWWWWWWWWWK.", "..KKKKKKKKKKKKK.",
};

static uint32_t ink(char ch) {
    switch (ch) {
    case 'K': return UI_BLACK;
    case 'W': return UI_WHITE;
    case 'L': return UI_LIGHT;
    case 'g': return UI_SHADE;
    case 'G': return UI_SHADOW;
    case 'D': return UI_DARK;
    case 'B': return 0x4466AA;
    case 'O': return 0x22AA22;
    case 'P': return 0xB8B8F0;          /* the Platinum folder's lavender */
    case 'p': return 0x7070C0;
    default: return 0xFFFFFFFFu;
    }
}
static void bitmap(ui_canvas *c, const char *const *rows, int x, int y) {
    for (int j = 0; j < 16; ++j)
        for (int i = 0; i < 16 && rows[j][i]; ++i) {
            const uint32_t v = ink(rows[j][i]);
            if (v != 0xFFFFFFFFu) ui_pixel(c, x + i, y + j, v);
        }
}
/* A filled disc of diameter d with a black edge. */
static void disc(ui_canvas *c, int x, int y, int d, uint32_t color) {
    const int r2 = d * d;
    for (int j = 0; j < d; ++j)
        for (int i = 0; i < d; ++i) {
            const int dx = 2 * i + 1 - d, dy = 2 * j + 1 - d, e = dx * dx + dy * dy;
            if (e <= r2) ui_pixel(c, x + i, y + j, e > (d - 2) * (d - 2) ? UI_BLACK : color);
        }
}
/* The caution triangle, size s (16 or 32). */
static void caution(ui_canvas *c, int x, int y, int s) {
    for (int j = 0; j < s - 1; ++j) {
        const int w = j + 1, l = x + (s - w) / 2;
        ui_fill(c, l, y + j, w, 1, UI_BLACK);
        if (w > 2 && j < s - 2) ui_fill(c, l + 1, y + j, w - 2, 1, 0xFFCC00);
    }
    ui_fill(c, x, y + s - 2, s, 1, UI_BLACK);
    const int bar = s / 16;
    ui_fill(c, x + s / 2 - bar, y + s * 5 / 16, 2 * bar, s * 6 / 16, UI_BLACK);
    ui_fill(c, x + s / 2 - bar, y + s * 12 / 16, 2 * bar, 2 * bar, UI_BLACK);
}

void ui_icon(ui_canvas *c, int icon, int x, int y, int large) {
    switch (icon) {
    case UI_ICON_HARD_DISK: bitmap(c, hard_disk, x, y); break;
    case UI_ICON_FLOPPY: bitmap(c, floppy, x, y); break;
    case UI_ICON_CD: bitmap(c, cd, x, y); break;
    case UI_ICON_MAC: bitmap(c, mac, x, y); break;
    case UI_ICON_FOLDER: bitmap(c, folder, x, y); break;
    case UI_ICON_DOCUMENT: bitmap(c, document, x, y); break;
    case UI_ICON_OK: {
        disc(c, x, y + 1, 14, 0x33AA33);
        for (int k = 0; k < 3; ++k) ui_fill(c, x + 3 + k, y + 7 + k, 2, 2, UI_WHITE);
        for (int k = 0; k < 5; ++k) ui_fill(c, x + 6 + k, y + 9 - k, 2, 2, UI_WHITE);
        break;
    }
    case UI_ICON_WARNING: caution(c, x, y, large ? 32 : 16); break;
    case UI_ICON_STOP: {
        const int s = large ? 32 : 16;
        disc(c, x, y, s, UI_ERROR);
        ui_fill(c, x + s / 4, y + s / 2 - s / 12, s / 2, s / 6 > 1 ? s / 6 : 2, UI_WHITE);
        break;
    }
    case UI_ICON_NOTE: {
        const int s = large ? 32 : 16;
        disc(c, x, y, s, UI_WHITE);
        ui_fill(c, x + s / 2 - s / 16, y + s * 7 / 16, s / 8, s * 6 / 16, UI_BLACK);
        ui_fill(c, x + s / 2 - s / 16, y + s * 3 / 16, s / 8, s / 8, UI_BLACK);
        break;
    }
    default: break;
    }
}
