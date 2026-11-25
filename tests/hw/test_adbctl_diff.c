/* Egret and Cuda: the protocol level against Apple's firmware (differential
 * harness). Both levels get the same packet sequences and ADB device
 * activity through the same VIA and host driver (adbctl_host.c); their
 * packets must match byte for byte and their timing within the tolerances
 * below, except for the divergence classes each step names. Skipped (77) when the firmware
 * dump is not in the ROM folder.
 *
 *   mc_test_adbctl_diff <firmware.bin> [-v]  */
#include "adbctl_host.h"
#include "../../src/core/romdb.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Divergence classes: a step may differ only in what its class allows. */
enum {
    EXACT = 0,
    CLOCK_PHASE = 1,        /* the clock's bytes may differ by the second the cold start lands in */
    TIMING_FREE = 2,        /* timing not compared (the firmware's loop phase decides it) */
    SELF_TEST = 4,          /* Egret's self-test result: the protocol level always passes; 344S0100's
                               dump fails its own ROM sum ($8D stored at $1E8C, $BD computed) */
};

static int verbose, failures;
static ac_host fw, pr;
static const double US = MC_C15M_HZ / 1000000.0;

static void show(const char *tag, const ac_packet *p) {
    printf("    %s", tag);
    if (!p) {
        printf(" (none)\n");
        return;
    }
    for (unsigned i = 0; i < p->n && i < 20; i++) printf(" %02X", p->b[i]);
    printf("\n");
}

static uint32_t clock_of(const ac_packet *p) {
    return (uint32_t)p->b[3] << 24 | (uint32_t)p->b[4] << 16 | (uint32_t)p->b[5] << 8 | p->b[6];
}

static int within(double a, double b, double rel, double abs_us) {
    const double d = a > b ? a - b : b - a;
    return d <= abs_us || d <= rel * (a > b ? a : b);
}

/* Compares two packets; `when` is the reference time of each (the end of
 * the host's packet or the event that caused them). */
static void compare(const char *what, const ac_packet *a, const ac_packet *b, uint64_t when_a, uint64_t when_b,
                    int cls) {
    int bad = 0;
    if (!a || !b) bad = a != b;
    else if (a->n != b->n) bad = 1;
    else if ((cls & CLOCK_PHASE) && a->n == 7) {
        const uint32_t ca = clock_of(a), cb = clock_of(b);
        bad = memcmp(a->b, b->b, 3) != 0 || (ca > cb ? ca - cb : cb - ca) > 1;
    } else if (cls & SELF_TEST) {
        bad = a->b[0] != b->b[0] || a->b[2] != b->b[2];
    } else bad = memcmp(a->b, b->b, a->n) != 0;
    if (!bad && a && b && !(cls & TIMING_FREE)) {
        const double la = (double)(a->attention - when_a) / US, lb = (double)(b->attention - when_b) / US;
        const double ba = a->n > 1 ? (double)(a->end - a->start) / (a->n - 1) / US : 0;
        const double bb = b->n > 1 ? (double)(b->end - b->start) / (b->n - 1) / US : 0;
        if (!within(la, lb, 0.10, 120.0) || !within(ba, bb, 0.15, 40.0)) {
            printf("  %s: timing differs: firmware %.0f us + %.0f us/byte, protocol %.0f us + %.0f us/byte\n", what, la,
                   ba, lb, bb);
            failures++;
        } else if (verbose) {
            printf("  %s: %.0f/%.0f us, %.0f/%.0f us/byte\n", what, la, lb, ba, bb);
        }
    }
    if (bad) {
        printf("  %s: packets differ\n", what);
        show("firmware", a);
        show("protocol", b);
        failures++;
    } else if (verbose) {
        show(what, a);
    }
}

static void command(const uint8_t *p, unsigned n, unsigned stream, int cls) {
    char what[64];
    int k = snprintf(what, sizeof what, "send");
    for (unsigned i = 0; i < n && k < 56; i++) k += snprintf(what + k, sizeof what - (size_t)k, " %02X", p[i]);
    fw.stream = pr.stream = stream;
    const ac_packet *a = ac_command(&fw, p, n, AC_MS(100));
    const ac_packet *b = ac_command(&pr, p, n, AC_MS(100));
    compare(what, a, b, fw.sent, pr.sent, cls);
}

#define CMD(cls, ...) do { const uint8_t pk_[] = {__VA_ARGS__}; command(pk_, sizeof pk_, 0, cls); } while (0)
#define STREAM(n, ...) do { const uint8_t pk_[] = {__VA_ARGS__}; command(pk_, sizeof pk_, n, EXACT); } while (0)

static void run_both(uint64_t periods) {
    ac_run(&fw, periods);
    ac_run(&pr, periods);
}

/* An ADB event on both machines, then the autopoll packets it brings. */
static void adb_event(const char *what, void (*act)(ac_host *), unsigned expect) {
    act(&fw);
    act(&pr);
    const uint64_t t_fw = fw.f.now, t_pr = pr.f.now;
    mc_adbctl_poke(&pr.ctl);
    for (unsigned i = 0; i < expect; i++) {
        const ac_packet *a = ac_next(&fw, AC_MS(200));
        const ac_packet *b = ac_next(&pr, AC_MS(200));
        char tag[64];
        snprintf(tag, sizeof tag, "%s (packet %u)", what, i + 1);
        compare(tag, a, b, t_fw, t_pr, TIMING_FREE);
        if (a && b) {
            /* Autopoll answers within two periods (11 ms) of the event. */
            const double da = (double)(a->attention - t_fw) / US, db = (double)(b->attention - t_pr) / US;
            if (da > 25000 || db > 25000) {
                printf("  %s: late: firmware %.0f us, protocol %.0f us\n", tag, da, db);
                failures++;
            } else if (verbose) {
                printf("  %s: after %.0f/%.0f us\n", tag, da, db);
            }
        }
    }
}

static void key_a_down(ac_host *h) { mc_adb_keyboard_key(&h->kbd, 0x00, 1); }
static void key_a_up(ac_host *h) { mc_adb_keyboard_key(&h->kbd, 0x00, 0); }
static void mouse_move(ac_host *h) { mc_adb_mouse_move(&h->mouse, 3, -2, 0); }

/* The one-second packets of a mode over `seconds`. */
static void ticks(uint8_t mode, unsigned seconds) {
    CMD(EXACT, 1, 0x1B, mode);
    const unsigned fw0 = fw.nlog, pr0 = pr.nlog;
    run_both(AC_MS(1000) * seconds + AC_MS(50));
    const unsigned nf = fw.nlog - fw0, np = pr.nlog - pr0;
    char tag[64];
    snprintf(tag, sizeof tag, "one-second packets, mode %u", mode);
    if (nf < seconds || np < seconds || (nf > np ? nf - np : np - nf) > 1) {
        printf("  %s: %u from the firmware, %u from the protocol level\n", tag, nf, np);
        failures++;
    }
    const unsigned n = nf < np ? nf : np;
    for (unsigned i = 0; i < n; i++)
        compare(tag, &fw.log[(fw0 + i) % AC_LOG], &pr.log[(pr0 + i) % AC_LOG], 0, 0, CLOCK_PHASE | TIMING_FREE);
    fw.read = fw.nlog;
    pr.read = pr.nlog;
    CMD(EXACT, 1, 0x1B, 0);
}

static void line_pulse(const char *what, int line) {
    const int a = fw.lines[line][1], b = pr.lines[line][1];
    if (!a || !b) {
        printf("  %s: line %d asserted %d times by the firmware, %d by the protocol level\n", what, line, a, b);
        failures++;
    } else if (verbose) {
        printf("  %s: line %d asserted\n", what, line);
    }
}

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : NULL;
    verbose = argc > 2;
    setvbuf(stdout, NULL, _IONBF, 0);
    if (!path) return 2;
    FILE *f = fopen(path, "rb");
    if (!f) {
        printf("skipped: %s absent\n", path);
        return 77;
    }
    static uint8_t image[MC_ADBCTL_FW_SIZE + 1];
    const size_t n = fread(image, 1, sizeof image, f);
    fclose(f);
    mc_adbctl_kind kind;
    const char *name = mc_adbctl_firmware_name(image, n, &kind);
    if (!name) {
        printf("%s: not a known firmware\n", path);
        return 1;
    }
    /* emulation.controller through the roms folders' index: the dump is
     * found by its CRC32 in its folder whatever its name; a folder without
     * it leaves auto on the protocol level and firmware refused. */
    {
        char folder[1024];
        snprintf(folder, sizeof folder, "%s", path);
        char *slash = strrchr(folder, '/'), *back = strrchr(folder, '\\');
        if (back > slash) slash = back;
        if (slash) slash[1] = 0; else folder[0] = 0;
        const char *folders[] = {folder};
        mc_rom_index ix;
        const mc_firmware *part = mc_firmware_identify(image, n);
        char key[64];
        snprintf(key, sizeof key, "%.*s", part ? (int)strlen(part->file) - 4 : 0, part ? part->file : "");
        const char *keys[] = {"egret-341s0850", key, NULL};
        const char *none[] = {NULL};
        static ac_host probe;
        int ok = part && mc_rom_index_scan(&ix, folders, 1);
        if (ok) {
            ac_init(&probe, kind, NULL);
            ok = mc_adbctl_select(&probe.ctl, MC_ADBCTL_FIRMWARE, &ix, keys) == 1 && probe.ctl.firmware;
            ac_init(&probe, kind, NULL);
            ok = ok && mc_adbctl_select(&probe.ctl, MC_ADBCTL_AUTO, &ix, none) == 0 && !probe.ctl.firmware;
            ok = ok && mc_adbctl_select(&probe.ctl, MC_ADBCTL_FIRMWARE, &ix, none) == -1;
            ok = ok && mc_adbctl_select(&probe.ctl, MC_ADBCTL_PROTOCOL, &ix, keys) == 0 && !probe.ctl.firmware;
            mc_rom_index_free(&ix);
        }
        if (!ok) {
            printf("%s: emulation.controller did not find it through the roms index\n", name);
            return 1;
        }
    }
    if (!ac_init(&fw, kind, image)) {
        printf("%s: the firmware did not come up\n", name);
        return 1;
    }
    ac_init(&pr, kind, NULL);
    mc_adbctl_set_part(&pr.ctl, name);
    const int cuda = kind == MC_ADBCTL_CUDA;
    printf("%s: up at %.3f s\n", name, fw.f.now / (double)MC_C15M_HZ);

    /* The clock. */
    CMD(EXACT, 1, 0x09, 0xA5, 0x00, 0x10, 0x20);
    CMD(CLOCK_PHASE, 1, 0x03);
    /* Parameter RAM and the address space. */
    CMD(EXACT, 1, 0x0C, 0x00, 0x10, 0x11, 0x22, 0x33, 0x44);
    STREAM(4, 1, 0x07, 0x00, 0x10);
    CMD(EXACT, 1, 0x0C, 0x00, 0xFF, 0x5A);
    STREAM(2, 1, 0x02, 0x01, 0xFF);
    STREAM(3, 1, 0x02, 0x00, 0xA4);
    CMD(EXACT, 1, 0x08, 0x01, 0x20, 0x77, 0x66);
    STREAM(2, 1, 0x07, 0x00, 0x20);
    CMD(EXACT, 1, 0x07, 0x02, 0x00);            /* a bad parameter RAM address */
    if (!cuda) STREAM(3, 1, 0x07, 0x01, 0x06);  /* Egret's 8 more bytes */
    /* Settings and their reads. */
    CMD(EXACT, 1, 0x14, 0x20);
    CMD(EXACT, 1, 0x16);
    CMD(EXACT, 1, 0x14, 0x0B);
    CMD(EXACT, 1, 0x19, 0x00, 0x0C);
    CMD(EXACT, 1, 0x1A);
    CMD(EXACT, 1, 0x0D, 0x00);
    CMD(EXACT, 1, 0x13, 0x00);
    CMD(EXACT, 1, 0x0B, 0x00, 0x00, 0x00, 0x00);
    CMD(EXACT, 1, 0x0E, 0x12);
    if (cuda) {
        CMD(EXACT, 1, 0x10);
        CMD(EXACT, 1, 0x12, 0x01);
        CMD(EXACT, 1, 0x21, 0x00);
        CMD(EXACT, 1, 0x21, 0x07);
        CMD(TIMING_FREE, 1, 0x22, 0x50, 0x00);  /* the IIC wait for an acknowledge */
        CMD(EXACT, 1, 0x24, 0x00);
        CMD(EXACT, 1, 0x04);
        CMD(EXACT, 1, 0x0F);
    } else {
        CMD(EXACT, 1, 0x04);
        CMD(EXACT, 1, 0x05);
        CMD(EXACT, 1, 0x06);
        CMD(EXACT, 1, 0x15);
        CMD(EXACT, 1, 0x17, 0x0F);
        CMD(EXACT, 1, 0x18);
        CMD(EXACT, 1, 0x1C, 0x00);
        CMD(EXACT, 1, 0x1D, 0x00);
        CMD(EXACT, 1, 0x1E, 0x02);
        CMD(EXACT, 1, 0x1F);
        CMD(EXACT, 1, 0x10);
        CMD(EXACT, 1, 0x12, 0x00);
        CMD(TIMING_FREE | SELF_TEST, 1, 0x0F);  /* the self-test's checksum loop */
    }
    /* Errors. */
    CMD(EXACT, 9, 0x09);
    CMD(EXACT, 1, 0x30);
    CMD(EXACT, 1, 0x03, 0x00);
    CMD(EXACT, 1, 0x09, 0x01);
    CMD(EXACT, 0, 0x2C, 0x01);
    CMD(EXACT, 0, 0x05);
    /* ADB. */
    CMD(EXACT, 0, 0x2F);
    CMD(EXACT, 0, 0x3F);
    CMD(EXACT, 0, 0x5F);
    CMD(EXACT, 0, 0x2C);
    CMD(EXACT, 0, 0x2A, 0xFF, 0xFB);
    CMD(EXACT, 0, 0x2E);
    CMD(EXACT, 0, 0x21);
    CMD(EXACT, 0, 0x3B, 0x68, 0x01);            /* the mouse to address 8 */
    CMD(EXACT, 0, 0x8F);
    CMD(TIMING_FREE, 0, 0x00);                  /* SendReset: back to 3 */
    CMD(EXACT, 0, 0x3F);
    /* A Talk while a device has data: its answer and the Service Request. */
    key_a_down(&fw);
    key_a_down(&pr);
    CMD(EXACT, 0, 0x3C);
    CMD(EXACT, 0, 0x2C);
    key_a_up(&fw);
    key_a_up(&pr);
    CMD(EXACT, 0, 0x2C);
    /* Autopoll. */
    CMD(EXACT, 1, 0x01, 0x01);
    run_both(AC_MS(30));
    adb_event("key down", key_a_down, 1);
    adb_event("key up", key_a_up, 1);
    adb_event("mouse", mouse_move, 1);
    run_both(AC_MS(30));
    CMD(EXACT, 1, 0x01, 0x00);
    /* One-second packets. */
    ticks(3, 3);
    ticks(2, 2);
    ticks(1, 2);
    /* Cuda: a resynchronisation turns autopoll and the one-second packets
     * off (341S0788 $15FB-$1603): nothing comes after it. */
    if (cuda) {
        CMD(EXACT, 1, 0x01, 0x01);
        CMD(EXACT, 1, 0x1B, 0x03);
        run_both(AC_MS(1100));
        fw.read = fw.nlog;
        pr.read = pr.nlog;
        ac_resync(&fw);
        ac_resync(&pr);
        const unsigned fw0 = fw.nlog, pr0 = pr.nlog;
        key_a_down(&fw);
        key_a_down(&pr);
        mc_adbctl_poke(&pr.ctl);
        run_both(AC_MS(2500));
        if (fw.nlog != fw0 || pr.nlog != pr0) {
            printf("  resynchronisation: %u packets after it from the firmware, %u from the protocol level\n", fw.nlog - fw0,
                   pr.nlog - pr0);
            failures++;
        }
        key_a_up(&fw);
        key_a_up(&pr);
        CMD(EXACT, 1, 0x16);
    }
    /* Restart: no reply, the 68k's reset pulsed, then the link works. */
    {
        const uint8_t p[] = {1, 0x11};
        memset(fw.lines, 0, sizeof fw.lines);
        memset(pr.lines, 0, sizeof pr.lines);
        ac_send(&fw, p, 2);
        ac_send(&pr, p, 2);
        run_both(AC_MS(3000));
        line_pulse("restart", MC_ADBCTL_LINE_RESET);
        CMD(TIMING_FREE, 1, 0x16);
    }
    /* Power down: the power line drops. */
    {
        const uint8_t p[] = {1, 0x0A};
        ac_send(&fw, p, 2);
        ac_send(&pr, p, 2);
        run_both(AC_MS(3000));
        if (fw.ctl.line_state[MC_ADBCTL_LINE_POWER] || pr.ctl.line_state[MC_ADBCTL_LINE_POWER]) {
            printf("  power down: the power stayed on (firmware %d, protocol %d)\n",
                   fw.ctl.line_state[MC_ADBCTL_LINE_POWER], pr.ctl.line_state[MC_ADBCTL_LINE_POWER]);
            failures++;
        }
    }
    printf("%s: %d divergence(s)\n", name, failures);
    return failures ? 1 : 0;
}
