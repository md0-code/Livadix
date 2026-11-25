/* The configuration window without a window (mc_config_ui_tests): every tab
 * drawn and saved as a BMP under work/tests/config-ui; a user's profiles
 * (a profile's own video keys taken out, its disks kept, no 0 x 0 screen);
 * a model chosen resets every setting that depends on it; switching
 * profiles shows exactly the profile chosen; disks, floppies, ports,
 * network and sharing edited; profiles made, renamed, deleted; a broken
 * profile does not hide the others; Save and Start enabled only
 * when they can act; the saved file read back by Livadix's own loader equal
 * to what the window showed, with the comments kept. Every file is made
 * here, under work/. */
#include "test.h"
#include "../src/app/config_app.h"
#include "../src/core/machine.h"
#include "../src/core/models.h"
#include "../src/core/profile.h"
#include "../src/core/compat.h"
#include "livadix_default_yaml.h"

#include "host.h"
#include <stdlib.h>
#ifdef _WIN32
#include <io.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>
#endif

int mc_test_failures;
static char dir[1024];

static const char *in_dir(const char *name) {
    static char path[4][1100];
    static unsigned next;
    char *p = path[next++ % 4];
    snprintf(p, 1100, "%s/%s", dir, name);
    return p;
}
static void write_file(const char *name, const void *data, size_t size) {
    FILE *f = fopen(in_dir(name), "wb");
    CHECK(f != NULL);
    if (!f) return;
    CHECK(fwrite(data, 1, size, f) == size);
    fclose(f);
}
static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *s = malloc((size_t)n + 1);
    if (s && fread(s, 1, (size_t)n, f) == (size_t)n) s[n] = 0;
    else { free(s); s = NULL; }
    fclose(f);
    return s;
}

/* --- driving the window -------------------------------------------------------------------- */

static void frame(cfg_app *a, ui_input in) { cfg_app_frame(a, &in, (ui_rect){0, 0, 0, 0}); }
static void idle(cfg_app *a) { ui_input in = {0}; frame(a, in); }
static void click(cfg_app *a, int x, int y) {
    ui_input in = {0};
    in.x = x; in.y = y;
    frame(a, in);
    in.pressed = 1;
    frame(a, in);
    in.pressed = 0; in.released = 1;
    frame(a, in);
    idle(a);
}
static int find(cfg_app *a, const char *label, ui_rect *r) {
    for (int i = 0; i < a->ui.hit_count; ++i)
        if (!strcmp(a->ui.hits[i].label, label)) { if (r) *r = a->ui.hits[i].r; return 1; }
    return 0;
}
/* A click with Ctrl or Shift held on the control labelled label. */
static void click_mods(cfg_app *a, const char *label, int mods) {
    ui_rect r;
    if (!find(a, label, &r)) { fprintf(stderr, "nothing labelled \"%s\"\n", label); mc_test_failures++; return; }
    ui_input in = {0};
    in.x = r.x + 8; in.y = r.y + r.h / 2; in.mods = mods;
    frame(a, in);
    in.pressed = 1;
    frame(a, in);
    in.pressed = 0; in.released = 1;
    frame(a, in);
    idle(a);
}
static int press(cfg_app *a, const char *label) {
    ui_rect r;
    /* An item of an open menu scrolled out of sight: scrolled to, as the mouse would. */
    if (!find(a, label, NULL) && ui_menu_reveal(&a->ui, label)) idle(a);
    if (!find(a, label, &r)) { fprintf(stderr, "nothing labelled \"%s\"\n", label); mc_test_failures++; return 0; }
    click(a, r.x + (r.w < 24 ? r.w / 2 : 8), r.y + r.h / 2);
    return 1;
}
static void choose(cfg_app *a, const char *popup, const char *item) {
    if (!press(a, popup)) return;
    CHECK(a->ui.menu.id != 0);
    if (!find(a, item, NULL)) fprintf(stderr, "the pop-up \"%s\" has no item \"%s\"\n", popup, item);
    press(a, item);
    if (a->ui.menu.id) { ui_input in = {0}; in.key = UI_KEY_ESCAPE; frame(a, in); idle(a); }
}
static void key(cfg_app *a, int k, int mods, int ch) {
    ui_input in = {0};
    in.key = k; in.mods = mods; in.ch = ch;
    frame(a, in);
    idle(a);
}
static void type(cfg_app *a, const char *text) {
    ui_input in = {0};
    snprintf(in.text, sizeof in.text, "%s", text);
    frame(a, in);
    idle(a);
}
/* Whether a control is drawn enabled: its pixels differ from the disabled
 * gray text... simpler: the toolkit registers disabled buttons too, so a
 * press on a disabled one changes nothing; enabled() asks the app. */
static void shot(cfg_app *a, const char *name) {
    const ui_canvas *c = &a->ui.canvas;
    const int stride = (c->w * 3 + 3) & ~3;
    const size_t size = 54 + (size_t)stride * c->h;
    uint8_t *b = calloc(1, size);
    if (!b) return;
    b[0] = 'B'; b[1] = 'M';
    const uint32_t fields[] = {(uint32_t)size, 0, 54, 40, (uint32_t)c->w, (uint32_t)c->h};
    for (int i = 0; i < 6; ++i) memcpy(b + 2 + 4 * i, &fields[i], 4);
    b[26] = 1; b[28] = 24;
    for (int y = 0; y < c->h; ++y)
        for (int x = 0; x < c->w; ++x) {
            const uint32_t p = c->px[(size_t)y * c->w + x];
            uint8_t *o = b + 54 + (size_t)(c->h - 1 - y) * stride + 3 * x;
            o[0] = (uint8_t)p; o[1] = (uint8_t)(p >> 8); o[2] = (uint8_t)(p >> 16);
        }
    write_file(name, b, size);
    free(b);
}
/* Sets the last four bytes of data so that its CRC-32 is target. */
static void force_crc(uint8_t *data, size_t size, uint32_t target) {
    uint32_t table[256], index[4] = {0};
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int k = 0; k < 8; ++k) c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
        table[i] = c;
    }
    uint32_t t = ~target;
    for (int k = 3; k >= 0; --k)
        for (uint32_t j = 0; j < 256; ++j)
            if (table[j] >> 24 == t >> 24) { index[k] = j; t = (t ^ table[j]) << 8; break; }
    uint32_t r = ~mc_crc32(data, size - 4);
    for (int k = 0; k < 4; ++k) {
        data[size - 4 + k] = (uint8_t)((r ^ index[k]) & 0xff);
        r = table[index[k]] ^ (r >> 8);
    }
}
static uint32_t pixel(cfg_app *a, int x, int y) { return a->ui.canvas.px[(size_t)y * a->ui.canvas.w + x]; }
/* How many of the canvas's pixels are not the window's gray. */
static size_t drawn(cfg_app *a, ui_rect r) {
    size_t n = 0;
    for (int y = r.y; y < r.y + r.h; ++y)
        for (int x = r.x; x < r.x + r.w; ++x) n += pixel(a, x, y) != UI_FACE;
    return n;
}
/* The whole document reads as Livadix reads it, every profile. */
static int document_reads(cfg_app *a) {
    size_t length = 0;
    char *text = mc_ydoc_text(&a->doc, &length), error[512] = "";
    mc_profile_list *list = malloc(sizeof *list);
    const int ok = text && list && mc_config_profiles_text(text, length, a->path, list, error, sizeof error);
    if (!ok) fprintf(stderr, "the document does not read: %s\n", error);
    free(list);
    free(text);
    return ok;
}
static const char *model_key(const cfg_app *a) { return mc_model_entry(a->config.model)->key; }

static int starts, resumes;
static char started_profile[MC_CONFIG_NAME];
static mc_config started_config;
static void start_livadix(void *ctx, const char *profile, const mc_config *config) {
    (void)ctx;
    starts++;
    snprintf(started_profile, sizeof started_profile, "%s", profile ? profile : "");
    started_config = *config;
}
static void resume(void *ctx) { (void)ctx; resumes++; }

/* A ROM file with the header checksum and size of a known ROM. */
static void make_rom(const char *name, uint32_t checksum) {
    size_t size = 0;
    for (size_t i = 0; i < mc_romdb_count(); ++i)
        if (mc_romdb_entry(i)->checksum == checksum) size = mc_romdb_entry(i)->size;
    CHECK(size != 0);
    uint8_t *rom = calloc(1, size);
    if (!rom) return;
    rom[0] = (uint8_t)(checksum >> 24); rom[1] = (uint8_t)(checksum >> 16); rom[2] = (uint8_t)(checksum >> 8); rom[3] = (uint8_t)checksum;
    write_file(name, rom, size);
    free(rom);
}

int main(void) {
    snprintf(dir, sizeof dir, "%s/tests/config-ui", MC_WORK);
    char parent[1100];
    snprintf(parent, sizeof parent, "%s/tests", MC_WORK);
    _mkdir(MC_WORK); _mkdir(parent); _mkdir(dir);
    _mkdir(in_dir("roms"));
    _mkdir(in_dir("disks"));
    /* ROMs under other names: the Quadra 700's, the Centris 650's, the
     * PowerBook 150's, the IIci's; and the LC 475's Cuda 341S0788. */
    make_rom("roms/q700.bin", 0x420DBFF3u);
    make_rom("roms/c650.bin", 0xF1A6F343u);
    make_rom("roms/powerbook-150.rom", 0xFDA22562u);
    make_rom("roms/iici.rom", 0x368CADFEu);
    make_rom("roms/pm6100.bin", 0x9FEB69B3u);         /* the Power Macintosh 6100's (PowerPC) */
    static uint8_t cuda[4352];
    for (size_t i = 0; i < sizeof cuda; ++i) cuda[i] = (uint8_t)(i * 11);
    force_crc(cuda, sizeof cuda, 0xdf6e1b43u);
    write_file("roms/my cuda.bin", cuda, sizeof cuda);
    /* The fonts tell 0 from 8 and O, 1 from l and I, 5 from S. */
    static const char *const pairs[][2] = {{"0", "8"}, {"0", "O"}, {"8", "B"}, {"1", "l"}, {"l", "I"}, {"1", "I"}, {"5", "S"},
                                           {"2", "Z"}};
    for (int f = 0; f < 2; ++f)
        for (size_t i = 0; i < sizeof pairs / sizeof *pairs; ++i) {
            uint32_t px[2][16 * 16];
            for (int k = 0; k < 2; ++k) {
                ui_canvas c = {px[k], 16, 16, {0, 0, 16, 16}};
                memset(px[k], 0, sizeof px[k]);
                ui_draw_text(&c, ui_font_get(f), 0, 0, pairs[i][k], 0xFFFFFF);
            }
            if (!memcmp(px[0], px[1], sizeof px[0])) {
                fprintf(stderr, "font %d draws \"%s\" as \"%s\"\n", f, pairs[i][0], pairs[i][1]);
                mc_test_failures++;
            }
        }
    /* Disk images: blank files (no System on them) beside the file. */
    static uint8_t blank[64 * 1024];
    write_file("disks/HD240.HF8", blank, sizeof blank);
    write_file("disks/second.img", blank, sizeof blank);
    /* A user's file: the template's shared sections with a shared ram_mb, a
     * Quadra 700 profile with its own video keys, the Centris 650 profile
     * the old editor broke, and a broken profile. */
    char text[32768];
    snprintf(text, sizeof text,
             "%.*s  # my own comment\n"
             "  \"Quadra 700 \xe2\x80\x93 Mac OS 8.0\":\n    model: quadra-700\n    storage:\n      disks:\n        - image: disks/second.img\n"
             "    video:\n      depth: 16\n      look: crt\n      monitor: color\n      resolution: 1024x768\n      card: livadix\n"
             "    emulation:\n      ram_mb: 68\n\n"
             "  \"Centris 650 \xe2\x80\x93 Mac OS 8.0\":\n    model: centris-650\n    emulation:\n      ram_mb: 24\n"
             "    storage:\n      disks:\n        - image: disks/HD240.HF8\n\n"
             "  \"IIci \xe2\x80\x93 System 7.5.3\":\n    model: iici\n    emulation: {ram_mb: 20}\n    video:\n      resolution: 640x480\n",
             (int)sizeof mc_default_yaml, (const char *)mc_default_yaml);
    remove(in_dir("livadix.yaml"));
    write_file("livadix.yaml", text, strlen(text));
    const char *q700 = "Quadra 700 \xe2\x80\x93 Mac OS 8.0", *c650 = "Centris 650 \xe2\x80\x93 Mac OS 8.0", *iici = "IIci \xe2\x80\x93 System 7.5.3";

    cfg_app *a = calloc(1, sizeof *a);
    const cfg_host host = {NULL, start_livadix, resume, NULL};
    CHECK(a && cfg_app_init(a, in_dir("livadix.yaml"), NULL, NULL, &host));
    if (!a) return 1;
    a->ui.canvas.w = 820; a->ui.canvas.h = 600;
    a->ui.canvas.px = calloc((size_t)a->ui.canvas.w * a->ui.canvas.h, sizeof *a->ui.canvas.px);
    idle(a);
    CHECK_EQ(a->profile_count, 5);
    CHECK(a->selected == 0 && !cfg_app_dirty(a));
    CHECK_STR(a->names[0], "Built-in");
    /* The window: its frame, the title bar's stripes, the default button's ring. */
    CHECK(pixel(a, 0, 0) == UI_BLACK && pixel(a, 100, 300) != 0);
    /* Every tab drawn. */
    static const char *const tabs[] = {"Machine", "Video", "Storage", "Sound", "Input", "Ports", "Network", "Sharing"};
    static const char *const marks[] = {"Model", "Resolution", "Floppy 1", "Play the Macintosh's sound", "Keyboard", "Modem port",
                                        "Ethernet", "Metadata"};
    for (int t = 0; t < 8; ++t) {
        press(a, tabs[t]);
        CHECK_EQ(a->tab, t);
        CHECK(find(a, marks[t], NULL));
        char name[64];
        snprintf(name, sizeof name, "tab-%s.bmp", tabs[t]);
        shot(a, name);
        CHECK(drawn(a, (ui_rect){260, 75, 520, 345}) > 300);
    }
    /* Save is disabled with nothing to save. */
    press(a, "Save");
    CHECK(!strstr(a->status, "Saved") && !cfg_app_dirty(a));

    /* The Centris 650 profile as written: its 24 MB, its disk, a screen. */
    CHECK(cfg_app_select(a, c650));
    idle(a);
    CHECK(a->config_ok && !strcmp(model_key(a), "centris-650") && a->config.ram_mb == 24);
    CHECK(a->config.disk_count == 1 && strstr(a->config.disks[0].image, "HD240.HF8"));
    CHECK(a->config.width == 640 && a->config.height == 480 && a->config.depth == 8);
    CHECK(!cfg_app_dirty(a));
    /* Its video edited: the profile's keys come and go, the file always
     * reads, the disks stay, the screen is never 0 x 0. */
    press(a, "Video");
    choose(a, "Resolution", "1024 x 768");
    CHECK(a->config.width == 1024 && a->config.height == 768 && a->config.depth == 8 && cfg_app_dirty(a));
    choose(a, "Depth", "16 bits (Thousands)");
    CHECK_EQ(a->config.depth, 16);
    choose(a, "Resolution", "640 x 480");
    choose(a, "Depth", "8 bits (256)");
    CHECK(a->config.width == 640 && a->config.depth == 8 && document_reads(a));
    {
        /* Back to the model's own: the profile's video keys go, and the
         * section with them (the old editor left "video:" empty). */
        const char *video[] = {"profiles", c650, "video"};
        CHECK(!mc_ydoc_has(&a->doc, video, 3));
    }
    CHECK(a->config_ok && a->config.disk_count == 1);
    /* A depth the display cannot show at a size is not offered; a size the
     * card shows only up to 8 bits takes the deepest it shows. */
    choose(a, "Resolution", "1920 x 1080");
    CHECK(a->config.width == 1920 && a->config.depth == 8);
    choose(a, "Depth", "32 bits (Millions)");
    CHECK(a->config.depth == 8);
    choose(a, "Resolution", "640 x 480");
    /* Built-in video: a size it does not show is not kept with it. */
    choose(a, "Display", "Built-in video");
    CHECK_EQ(a->config.video_card, MC_VIDEO_CARD_BUILTIN);
    choose(a, "Resolution", "800 x 600");
    CHECK(a->config.width == 800 && a->config.video_card == MC_VIDEO_CARD_AUTO);
    choose(a, "Display", "Livadix Display Card");
    choose(a, "Depth", "32 bits (Millions)");
    CHECK(a->config.video_card == MC_VIDEO_CARD_LIVADIX && a->config.depth == 32);
    choose(a, "Look", "CRT");
    CHECK(a->config.look == MC_LOOK_CRT && document_reads(a));
    shot(a, "video-centris.bmp");

    /* A model chosen: every setting that depends on it is the model's own,
     * nothing of the Centris stays (its card, depth, look, memory). */
    press(a, "Machine");
    choose(a, "Model", "Macintosh Quadra 700");
    const mc_model *m700 = mc_model_find("quadra-700");
    CHECK(!strcmp(model_key(a), "quadra-700"));
    CHECK(a->config.width == m700->width && a->config.height == m700->height && a->config.depth == m700->depth);
    CHECK(a->config.video_card == MC_VIDEO_CARD_AUTO && a->config.look == m700->look && a->config.monitor == m700->monitor);
    CHECK(a->config.ram_mb == 4 && a->config.cpu_hz == m700->cpu_hz);
    CHECK(a->config.disk_count == 1 && document_reads(a));
    /* Memory: only sizes the Quadra 700 takes. */
    choose(a, "Memory", "20 MB");
    CHECK_EQ(a->config.ram_mb, 20);
    CHECK(!find(a, "24 MB", NULL));
    /* Back to the Centris: its own settings again. */
    choose(a, "Model", "Macintosh Centris 650");
    const mc_model *m650 = mc_model_find("centris-650");
    CHECK(a->config.ram_mb == m650->ram_mb && a->config.width == 640 && a->config.depth == 8 && a->config.look == MC_LOOK_PLAIN);
    /* The CPU speed: the model's clock, or a speed of one's own. */
    CHECK(find(a, "The model's clock (25 MHz)", NULL));
    press(a, "The model's clock (25 MHz)");
    CHECK(a->custom_mhz);
    ui_focus(&a->ui, ui_id("mhz-text", 0));
    for (int i = 0; i < 4; ++i) key(a, UI_KEY_BACKSPACE, 0, 0);
    type(a, "40");
    CHECK_EQ(a->config.cpu_hz, 40000000);
    press(a, "The model's clock (25 MHz)");
    CHECK(!a->custom_mhz && a->config.cpu_hz == 25000000);
    /* Closing the window: shut down first (the default), or quit at once. */
    CHECK(a->config.close_action == MC_CLOSE_SHUTDOWN);
    press(a, "Shutdown of the Macintosh before the window closes");
    CHECK(a->config.close_action == MC_CLOSE_QUIT);
    press(a, "Shutdown of the Macintosh before the window closes");
    shot(a, "machine-centris.bmp");

    /* Switching profiles shows exactly the profile chosen. */
    CHECK(cfg_app_select(a, q700));
    idle(a);
    CHECK(!strcmp(model_key(a), "quadra-700") && a->config.ram_mb == 68);
    CHECK(a->config.width == 1024 && a->config.height == 768 && a->config.depth == 16);
    CHECK(a->config.video_card == MC_VIDEO_CARD_LIVADIX && a->config.look == MC_LOOK_CRT);
    CHECK(a->config.disk_count == 1 && strstr(a->config.disks[0].image, "second.img"));
    CHECK_STR(a->mhz_text, "25");
    CHECK(cfg_app_select(a, c650));
    idle(a);
    CHECK(!strcmp(model_key(a), "centris-650") && a->config.ram_mb == m650->ram_mb && a->config.look == MC_LOOK_PLAIN);
    CHECK(a->config.disk_count == 1 && strstr(a->config.disks[0].image, "HD240.HF8") && a->config.video_card == MC_VIDEO_CARD_AUTO);

    /* Storage: a disk added through the chooser, its SCSI ID and lock; the
     * same image twice is refused. */
    press(a, "Storage");
    press(a, "Add\xe2\x80\xa6");
    /* the picker opens in the folder of the disk there; its image chosen and opened */
    CHECK(a->pending_pick == CFG_PICK_DISK + 1 && a->dialog == CFG_DIALOG_PICK);
    CHECK(strstr(a->picker.folder, "disks") && a->picker.shown_count == 2);
    press(a, "second.img");
    press(a, "Open");
    CHECK(a->dialog == CFG_DIALOG_NONE && !a->pending_pick);
    CHECK(a->config.disk_count == 2 && a->disk_selected == 1);
    choose(a, "SCSI ID", "5");
    CHECK(a->config.disks[1].scsi_id_set && a->config.disks[1].scsi_id == 5);
    press(a, "Read-only (never written)");
    CHECK(a->config.disks[1].readonly);
    press(a, "Add\xe2\x80\xa6");
    cfg_app_pick_result(a, in_dir("disks/second.img"));
    idle(a);
    CHECK(a->config.disk_count == 2 && strstr(a->status, "in a drive already"));
    press(a, "Move Up");
    CHECK(a->disk_selected == 0 && strstr(a->config.disks[0].image, "second.img"));
    shot(a, "storage-added.bmp");
    /* A floppy dropped on the window goes into the first drive; the second
     * drive fitted empty. */
    static uint8_t floppy[800 * 1024];
    write_file("floppy.img", floppy, sizeof floppy);
    cfg_app_drop(a, in_dir("floppy.img"));
    idle(a);
    CHECK(strstr(a->config.floppies[0].image, "floppy.img") != NULL);
    choose(a, "Floppy 2", "Empty");
    CHECK(a->config.floppy_count == 2 && !a->config.floppy_off[1]);
    choose(a, "Floppy 2", "Not fitted");
    CHECK(a->config.floppy_count == 1);
    CHECK(document_reads(a));
    /* Input and ports. */
    press(a, "Input");
    choose(a, "Keyboard", "ISO");
    choose(a, "Command key", "Right Alt");
    choose(a, "Right button", "Control-click");
    CHECK(a->config.keyboard_layout == MC_KEYBOARD_ISO && a->config.command_key == MC_COMMAND_RIGHT_ALT &&
          a->config.mouse[1] == MC_MOUSE_CONTROL_CLICK);
    /* The left button cannot stop clicking while the right one does not. */
    choose(a, "Left button", "Disabled");
    CHECK(a->config.mouse[0] == MC_MOUSE_NORMAL);
    press(a, "Ports");
    choose(a, "Printer port", "Named pipe");
    CHECK(a->config.ports[1].type == MC_PORT_PIPE && !strcmp(a->pipe_text[1], TEST_PIPE));
    choose(a, "Modem port", "Output file");
    CHECK(a->pending_pick == CFG_PICK_OUTPUT + 1);
    cfg_app_pick_result(a, in_dir("modem.out"));
    idle(a);
    CHECK(a->config.ports[0].type == MC_PORT_OUTPUT);
    /* The network: no cable, then NAT again with forwards typed in; the
     * card the model's slot takes only. */
    press(a, "Network");
    choose(a, "Ethernet", "None (no cable)");
    CHECK(a->config.network == MC_NETWORK_NONE);
    choose(a, "Ethernet", "NAT (the Internet through this computer)");
    ui_focus(&a->ui, ui_id("forwards", 0));
    type(a, "tcp:8080:80, ");
    type(a, "udp:0.0.0.0:5353:53");
    key(a, UI_KEY_ENTER, 0, 0);
    CHECK(a->config.forward_count == 2 && a->config.forwards[0].host_port == 8080 && a->config.forwards[1].udp &&
          a->config.forwards[1].address == 0 && a->config.forwards[1].guest_port == 53);
    /* Sharing: a folder added through the chooser, read-only, AppleDouble. */
    press(a, "Sharing");
    press(a, "Add\xe2\x80\xa6");
    cfg_app_pick_result(a, dir);
    idle(a);
    CHECK_EQ(a->config.share_count, 1);
    CHECK_STR(a->config.shares[0].name, "config-ui");
    press(a, "Read-only");
    CHECK(a->config.shares[0].readonly);
    choose(a, "Metadata", "AppleDouble (._ files)");
    CHECK_EQ(a->config.sharing_metadata, MC_SHARING_APPLEDOUBLE);
    shot(a, "tab-Sharing-folder.bmp");
    CHECK(document_reads(a));

    /* The IIci: the RBV's sizes, and a monochrome display takes one of
     * Apple's monochrome modes. */
    CHECK(cfg_app_select(a, iici));
    idle(a);
    press(a, "Video");
    choose(a, "Resolution", "1152 x 870");
    choose(a, "Depth", "16 bits (Thousands)");
    CHECK(a->config.width == 1152 && a->config.depth == 16);
    choose(a, "Monitor", "White (monochrome CRT)");
    CHECK(a->config.monitor == MC_MONITOR_MONO && a->config.width == 1152 && a->config.depth == 8);
    choose(a, "Resolution", "800 x 600");
    CHECK(a->config.width == 800 && a->config.monitor == MC_MONITOR_COLOR);

    /* The PowerBook 150: its panel is the only screen, the internal IDE
     * disk takes the first disk. */
    CHECK(cfg_app_select(a, c650));
    press(a, "Machine");
    choose(a, "Model", "PowerBook 150");
    CHECK(!strcmp(model_key(a), "powerbook-150") && a->config.width == 640 && a->config.height == 480 && a->config.disks[0].bus == MC_BUS_ATA);
    press(a, "Video");
    choose(a, "Resolution", "1024 x 768");
    CHECK(a->config.width == 640);
    CHECK(document_reads(a));
    press(a, "Machine");
    /* PowerPC models and their own clocks/memory; the 68040 FPU's
     * option does not apply. The Workgroup Servers are selectable too. */
    press(a, "Model");
    CHECK(ui_menu_reveal(&a->ui, "Power Macintosh 8100") && ui_menu_reveal(&a->ui, "Workgroup Server 6150"));
    { ui_input in = {0}; in.key = UI_KEY_ESCAPE; frame(a, in); idle(a); }
    choose(a, "Model", "Power Macintosh 7100");
    CHECK(!strcmp(model_key(a), "power-macintosh-7100") && a->config.cpu_hz == 66000000u && a->config.ram_mb == 8 && !a->config.fpu_full);
    CHECK(find(a, "The model's clock (66 MHz)", NULL));
    press(a, "Full 68881 instruction set in the 68040's FPU");
    CHECK(!a->config.fpu_full);
    choose(a, "Model", "Workgroup Server 6150");
    CHECK(!strcmp(model_key(a), "workgroup-server-6150"));
    CHECK(document_reads(a));
    choose(a, "Model", "Macintosh Centris 650");
    CHECK(a->config.disks[0].bus == MC_BUS_SCSI);
    /* A menu taller than the window scrolls (End shows the last item); the
     * filters narrow the Model pop-up. */
    press(a, "Model");
    if (a->ui.menu.count > 40) {
        CHECK(a->ui.menu.visible < a->ui.menu.count);
        key(a, UI_KEY_END, 0, 0);
        CHECK(a->ui.menu.first + a->ui.menu.visible == a->ui.menu.count);
    }
    { ui_input in = {0}; in.key = UI_KEY_ESCAPE; frame(a, in); idle(a); }
    choose(a, "Type", "Laptop");
    press(a, "Model");
    /* The chosen Centris 650 is no laptop: the first laptop listed takes its place. */
    CHECK(!strncmp(model_key(a), "powerbook", 9));
    CHECK(ui_menu_reveal(&a->ui, "PowerBook 150") && !ui_menu_reveal(&a->ui, "Macintosh Quadra 700") &&
          !ui_menu_reveal(&a->ui, "Macintosh Centris 650"));
    { ui_input in = {0}; in.key = UI_KEY_ESCAPE; frame(a, in); idle(a); }
    shot(a, "machine-filtered.bmp");
    /* A value no model has with the others clears them: no PowerPC laptop here. */
    choose(a, "Architecture", "PowerPC");
    CHECK(a->filter_family == 2 && a->filter_form == 0);
    choose(a, "Architecture", "Any");
    choose(a, "Type", "Any");
    choose(a, "Model", "Macintosh Centris 650");
    /* The selected model's Systems. */
    CHECK(find(a, "Runs System 7.1 to Mac OS 8.1; before 7.5 with System Enabler 040", NULL));

    /* Profiles: new (the Built-in machine's own settings), duplicate,
     * rename, default, delete. */
    press(a, "New\xe2\x80\xa6");
    CHECK(a->dialog == CFG_DIALOG_NEW);
    shot(a, "dialog-new.bmp");
    for (int i = 0; i < 11; ++i) key(a, UI_KEY_BACKSPACE, 0, 0);
    type(a, "Scratch");
    key(a, UI_KEY_ENTER, 0, 0);
    CHECK(a->dialog == CFG_DIALOG_NONE && a->profile_count == 6);
    CHECK_STR(a->names[a->selected], "Scratch");
    CHECK(!strcmp(model_key(a), MC_MODEL_BUILTIN) && a->config_ok && a->config.disk_count == 0 && document_reads(a));
    press(a, "Duplicate\xe2\x80\xa6");
    press(a, "OK");
    CHECK(a->profile_count == 7);
    CHECK_STR(a->names[a->selected], "Scratch copy");
    press(a, "Rename\xe2\x80\xa6");
    type(a, " 2");
    press(a, "OK");
    CHECK_STR(a->names[a->selected], "Scratch copy 2");
    press(a, "Make Default");
    CHECK(a->default_index == a->selected);
    press(a, "Delete\xe2\x80\xa6");
    shot(a, "dialog-delete.bmp");
    press(a, "Delete");
    CHECK(a->profile_count == 6 && a->default_index == 0);
    CHECK(document_reads(a));
    /* Several at once: Ctrl-click adds one, Shift-click a range from the
     * last click; Rename wants one; Delete takes them all. */
    CHECK(cfg_app_select(a, "Scratch"));
    click_mods(a, "Mac OS 8.0", UI_MOD_CTRL);
    CHECK(a->marked[1] && a->marked[5] && !a->marked[2] && !strcmp(cfg_app_selected(a), "Mac OS 8.0"));
    press(a, "Rename\xe2\x80\xa6");
    CHECK(a->dialog == CFG_DIALOG_NONE);
    click_mods(a, "Built-in", UI_MOD_SHIFT);
    CHECK(a->marked[0] && a->marked[1] && !a->marked[5]);
    click_mods(a, "Scratch", UI_MOD_CTRL);
    press(a, "Delete\xe2\x80\xa6");
    press(a, "Delete");
    CHECK(a->profile_count == 3 && a->default_index == 0 && document_reads(a));

    /* Start: with a disk image missing it cannot act; with every image
     * there it starts the profile as edited. */
    CHECK(cfg_app_select(a, c650));
    idle(a);
    remove(in_dir("disks/HD240.HF8"));
    idle(a);
    press(a, "Start");
    CHECK(starts == 0);
    write_file("disks/HD240.HF8", blank, sizeof blank);
    idle(a);
    press(a, "Start");
    CHECK(starts == 1 && !strcmp(started_profile, c650) && mc_profile_same(&started_config, &a->config));
    /* Close goes back to the Macintosh: unsaved changes are warned about
     * first, the same ones once; they stay. */
    const int before = resumes;
    press(a, "Close");
    CHECK(a->dialog == CFG_DIALOG_CLOSE && resumes == before);
    press(a, "Don't Save");
    CHECK(a->dialog == CFG_DIALOG_NONE && resumes == before + 1 && cfg_app_dirty(a));
    press(a, "Close");
    CHECK(a->dialog == CFG_DIALOG_NONE && resumes == before + 2);

    /* Save: Livadix's loader reads each profile as the window showed it; the
     * comments and the profiles not edited are kept. */
    key(a, UI_KEY_CHAR, UI_MOD_CTRL, 's');
    CHECK(!cfg_app_dirty(a) && strstr(a->status, "Saved"));
    char *saved = read_file(in_dir("livadix.yaml"));
    CHECK(saved && strstr(saved, "  # my own comment\n"));
    CHECK(saved && strstr(saved, "# PROFILES: each one a name and its settings."));
    CHECK(saved && strstr(saved, "    emulation: {ram_mb: 20}\n    video:\n      resolution: 640x480\n") == NULL);
    for (size_t i = 0; i < a->profile_count; ++i) {
        mc_config shown, loaded;
        char error[512];
        CHECK(cfg_app_select(a, a->names[i]));
        shown = a->config;
        CHECK(mc_config_load_profile(in_dir("livadix.yaml"), a->names[i], &loaded, error, sizeof error));
        CHECK(mc_profile_same(&shown, &loaded));
    }
    CHECK(!cfg_app_dirty(a));
    /* Opened again: the same document. */
    cfg_app *b = calloc(1, sizeof *b);
    CHECK(b && cfg_app_init(b, in_dir("livadix.yaml"), NULL, NULL, &host));
    if (b) {
        char *x = mc_ydoc_text(&a->doc, NULL), *y = mc_ydoc_text(&b->doc, NULL);
        CHECK(x && y && !strcmp(x, y) && !cfg_app_dirty(b));
        free(x); free(y);
        cfg_app_free(b);
        free(b);
    }
    /* A broken profile does not hide the others; a change writes it anew. */
    char broken[40000];
    snprintf(broken, sizeof broken, "%s  Broken:\n    model: quadra-700\n    video:\n", saved ? saved : "");
    write_file("livadix.yaml", broken, strlen(broken));
    cfg_app *c = calloc(1, sizeof *c);
    CHECK(c && cfg_app_init(c, in_dir("livadix.yaml"), NULL, NULL, &host));
    if (c) {
        c->ui.canvas = a->ui.canvas;
        CHECK(c->profile_count == a->profile_count + 1 && !c->doc_error[0]);
        CHECK(cfg_app_select(c, c650) && c->config_ok && c->config.disk_count == 2);
        CHECK(cfg_app_select(c, "Broken") && !c->config_ok && strstr(c->config_error, "Expected a mapping"));
        idle(c);
        press(c, "Video");
        choose(c, "Depth", "4 bits (16)");
        CHECK(c->config_ok && c->config.depth == 4 && document_reads(c));
        c->ui.canvas.px = NULL;
        cfg_app_free(c);
        free(c);
    }
    /* Resized: the layout follows. */
    free(a->ui.canvas.px);
    a->ui.canvas.w = 1100; a->ui.canvas.h = 760;
    a->ui.canvas.px = calloc((size_t)a->ui.canvas.w * a->ui.canvas.h, sizeof *a->ui.canvas.px);
    idle(a);
    ui_rect save;
    CHECK(find(a, "Save", &save) && save.x > 900);
    shot(a, "resized.bmp");
    /* The UI must accept the same >4 GiB disks as the disk backend. */
    FILE *large=fopen(in_dir("disks/large.img"),"w+b");CHECK(large!=NULL);
    if(large){
#ifdef _WIN32
        DWORD returned;HANDLE h=(HANDLE)_get_osfhandle(_fileno(large));
        CHECK(DeviceIoControl(h,FSCTL_SET_SPARSE,NULL,0,NULL,0,&returned,NULL));
        CHECK(_chsize_s(_fileno(large),6000000000ll)==0);fclose(large);
#else
        CHECK(ftruncate(fileno(large),6000000000ll)==0);fclose(large);
#endif
        CHECK(cfg_app_select(a,c650));a->config.disk_count=1;
        snprintf(a->config.disks[0].image,sizeof a->config.disks[0].image,"%s",in_dir("disks/large.img"));
        a->tab=CFG_TAB_STORAGE;idle(a);
        int cached=0;for(unsigned i=0;i<CFG_VOLUME_CACHE;++i)
            if(!strcmp(a->cache[i].path,a->config.disks[0].image)&&a->cache[i].ok&&a->cache[i].info.bytes==6000000000ull)cached=1;
        CHECK(cached);int prior=starts;press(a,"Start");CHECK(starts==prior+1);
        shot(a,"large-image.bmp");remove(in_dir("disks/large.img"));
    }
    char systems[512];mc_compat_describe(mc_model_find("power-macintosh-g3"),systems,sizeof systems);
    CHECK(strstr(systems,"Mac OS X 10.0 to 10.2.8")!=NULL);
    free(saved);
    free(a->ui.canvas.px);
    cfg_app_free(a);
    free(a);
    printf("config-ui: %d failure(s); screenshots in %s\n", mc_test_failures, dir);
    return mc_test_failures ? 1 : 0;
}
