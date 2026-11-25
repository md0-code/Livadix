/* pdm_boot: a PowerPC Macintosh (the 6100, 7100 and 8100 of machines/pdm.c,
 * the first PCI generation of machines/pci1.c, and the Capella boards) run headless from reset with
 * disk images, reporting where it is and saving the screen; diagnosis and
 * the boot checks.
 *
 *   mc_ppc_pdm_boot ROM MODEL SECONDS [options]
 *     MODEL 6100, 7100 or 8100 (the PDM boards), or a catalogue model's
 *     number or key (7500, power-macintosh-8600); SECONDS of guest time
 *     --ram MB        RAM (default 16)
 *     --disk IMAGE    a hard disk on SCSI (repeatable;
 *                     images open writable unless --ro)
 *     --ata / --scsi  explicitly select the hard disks' bus (default SCSI)
 *     --cd IMAGE      the CD-ROM drive's disc
 *     --floppy IMAGE
 *     --cuda-fw DIR   Cuda's firmware from the roms folder DIR
 *     --nvram FILE    the PRAM file (loaded, and saved at the end)
 *     --key CODE@S    an ADB key (hex) pressed at S guest seconds
 *     --insert-floppy S IMAGE   a floppy inserted at S guest seconds
 *     --insert-cd S IMAGE       a CD inserted at S guest seconds
 *     --jit           the PowerPC translator (emulation.jit)
 *     --jit-profile FILE  opt-in host timings and call-out counts as CSV
 *     --ro            open the images read-only
 *     --size WxH      the monitor (default 640x480)
 *     --depth BITS    configured video depth (default 8)
 *     --expect-screen fail unless the machine shows a screen at the end
 *     --card          the Livadix Display Card instead of the built-in video
 *     --accel on|off  the card's blitter (video.acceleration; default on)
 *     --raw FILE      the screen's raw pixels at the end, its rows without
 *                     padding (at the depth shown)
 *     --until-found WORD S  end S guest seconds after the long WORD (hex)
 *                     shows in RAM (looked for once a guest second), not as
 *                     the immediate of a 68k move.l #,(An) ($20BC + An)
 *     --shot FILE     a BMP of the screen at the end
 *     --every S       report every S guest seconds
 *     --io-range LOW HIGH  limit diagnostic I/O trace to hex addresses
 *     --control        stdin: run S, shot FILE, key HEX DOWN, mouse DX DY BUTTON, mouse-to X Y, type TEXT, state, ram FILE, video, peek ADDR WORDS, poke ADDR WORD, floppy FILE, quit
 *     --cd-ata / --cd-scsi  select the optical drive bus
 *     --until-menu-bar S  end S guest seconds after the Finder's menu bar
 *                     shows (looked for once a guest second), else at SECONDS
 *     --shots PREFIX  a BMP of the screen at every report (PREFIXsss.bmp)
 *     --dump ADDR LEN words of guest memory (hex) at the end
 *     --dump-sp LEN   words from the PPC stack pointer r1 (hex length)
 *     --stop-pc PC   stop at a sampled PowerPC instruction address (hex)
 *     --stop-pc-range LO HI stop within the sampled PPC range [LO, HI)
 *     --stop-pc68k PC stop at the 68k emulator's PC (r24, hex), sampled
 *                     between runs; use --fine for instruction-sized slices
 *     --stop-exception VECTOR stop at a PPC exception vector (hex), sampled
 *                     between runs (SRR0, SRR1, DAR and DSISR printed); with
 *                     --fine, only from its S time
 *     --io N          print the first N I/O accesses (the bus's slow path)
 *     --audio-input FILE   raw interleaved stereo signed 16-bit LE input at the guest rate
 *     --wav FILE      the sound output as a WAV file ("audio peak" printed)
 *     --no-chime      silence only the startup chime (emulation.startup_sound)
 *     --net           the Ethernet on Livadix's NAT (network.mode nat); its
 *                     counters at the end
 *     --volumes       at the end, the mounted volumes ("volume: NAME")
 *     --find-a5       also read a matching 68k A5 result through its virtual mapping
 *     --find WORD LEN at the end, every copy of the long WORD (hex) in RAM
 *                     with LEN (hex) bytes from it and the word before it
 *     --watch ADDR    report each change of the word at ADDR (hex) with the
 *                     PC and the 68k emulator's PC (r24)
 *     --fine S N      from S guest seconds on, run N instructions at a time
 *     --pc68k LO HI   report (four times) when the 68k emulator's PC (r24)
 *                     is in [LO, HI) between runs: d0-d7, a0-a7 (r8-r23)
 *                     and the 68k stack
 *     --pc68k-hits N  report N times instead of four
 *     --mhz MHZ       the processor clock (a catalogue model's by default)
 *     --serial        print what the serial ports send (Open Firmware's ttya)
 *     --serial-loopback  check the PBX serial probe on the modem/printer port
 *     --type TEXT@S   type TEXT into the modem port from S guest seconds, a
 *                     character per run slice (backslash-r: Return)
 *     --hold CODE@S   hold an ADB key (hex) from the start to S guest seconds
 *                     (Command-Option-O-F: 37, 3A, 1F, 03 for Open Firmware)
 *
 * At the end it prints the card's blitter operations ("blitter: N
 * operations"), the box flag (low memory $CB3: Gestalt's machine
 * type less 6, ROM $FFD757EE) and "powered off" when the machine switched
 * itself off.
 *
 * Livadix's own code. */
#include "ppc/ppc_machine.h"
#include "ppc/m68k_accel.h"
#include "../../src/core/config.h"
#include "../../src/core/disk.h"
#include "../../src/core/models.h"
#include "../../src/core/romdb.h"
#include "../../src/net/net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void save_bmp(const char *path, const uint32_t *argb, uint32_t w, uint32_t h) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    const uint32_t stride = (w * 3 + 3) & ~3u, size = 54 + stride * h;
    uint8_t hdr[54] = {'B', 'M'};
    for (int i = 0; i < 4; i++) {
        hdr[2 + i] = (uint8_t)(size >> (8 * i));
        hdr[18 + i] = (uint8_t)(w >> (8 * i));
        hdr[22 + i] = (uint8_t)(h >> (8 * i));
    }
    hdr[10] = 54;
    hdr[14] = 40;
    hdr[26] = 1;
    hdr[28] = 24;
    fwrite(hdr, 1, 54, f);
    uint8_t *row = calloc(1, stride);
    for (uint32_t y = h; y-- > 0;) {
        for (uint32_t x = 0; x < w; x++) {
            const uint32_t p = argb[(size_t)y * w + x];
            row[x * 3] = (uint8_t)p;
            row[x * 3 + 1] = (uint8_t)(p >> 8);
            row[x * 3 + 2] = (uint8_t)(p >> 16);
        }
        fwrite(row, 1, stride, f);
    }
    free(row);
    fclose(f);
}

/* A BMP of the screen; 0 when there is none. */
static int shoot(mc_ppc *m, const char *path) {
    mc_framebuffer fb;
    memset(&fb, 0, sizeof fb);
    if (!mc_ppc_framebuffer(m, &fb)) return 0;
    uint32_t *argb = malloc((size_t)fb.width * fb.height * 4);
    mc_framebuffer_argb(&fb, argb);
    save_bmp(path, argb, fb.width, fb.height);
    free(argb);
    return 1;
}

/* A menu bar across the top, as the Finder draws it once it runs (the same
 * test as tests/verify.py's finder_menu_bar): row 19 nearly all dark, row 1
 * light, row 10 partly dark (the titles). */
static int menu_bar(mc_ppc *m) {
    mc_framebuffer fb;
    memset(&fb, 0, sizeof fb);
    if (!mc_ppc_framebuffer(m, &fb) || fb.height < 20 || fb.width < 64) return 0;
    uint32_t *argb = malloc((size_t)fb.width * fb.height * 4);
    if (!argb) return 0;
    mc_framebuffer_argb(&fb, argb);
    unsigned dark19 = 0, light1 = 0, dark10 = 0, title_ink = 0;
    for (unsigned x = 0; x < fb.width; x++) {
        const uint32_t a = argb[19u * fb.width + x], b = argb[1u * fb.width + x], c = argb[10u * fb.width + x];
        const unsigned amax = (a >> 16 & 255) > (a >> 8 & 255) ? (a >> 16 & 255) : (a >> 8 & 255);
        const unsigned cmax = (c >> 16 & 255) > (c >> 8 & 255) ? (c >> 16 & 255) : (c >> 8 & 255);
        const unsigned bmin = (b >> 16 & 255) < (b >> 8 & 255) ? (b >> 16 & 255) : (b >> 8 & 255);
        dark19 += (amax > (a & 255) ? amax : (a & 255)) <= 0x40;
        light1 += (bmin < (b & 255) ? bmin : (b & 255)) >= 0xC0;
        dark10 += (cmax > (c & 255) ? cmax : (c & 255)) <= 0x40;
        if (x >= 24u && x < fb.width / 2u && x < 320u)
            title_ink += (cmax > (c & 255) ? cmax : (c & 255)) <= 0x40;
    }
    free(argb);
    return dark19 * 100 >= fb.width * 98u && light1 * 10 >= fb.width * 9u && dark10 * 50 >= fb.width && dark10 * 5 <= fb.width * 2u && title_ink >= 18u;
}

/* The sound output, 16-bit stereo, kept whole for the WAV file. */
static void input_pcm(void *ctx, unsigned rate, int16_t frame[2]) {
    uint8_t raw[4];
    (void)rate; /* the fixture is already sampled at the guest rate */
    if (fread(raw, 1, sizeof raw, ctx) == sizeof raw) {
        frame[0] = (int16_t)((uint16_t)raw[0] | (uint16_t)raw[1] << 8);
        frame[1] = (int16_t)((uint16_t)raw[2] | (uint16_t)raw[3] << 8);
    }
}

static int16_t *audio;
static size_t audio_frames, audio_cap;
static int audio_peak;
static void pull_audio(mc_ppc *m) {
    int16_t buf[2 * 4096];
    size_t n;
    while ((n = mc_ppc_audio_pull(m, buf, 4096)) > 0) {
        if (audio_frames + n > audio_cap) {
            size_t cap = audio_cap ? audio_cap * 2 : 1u << 20;
            while (cap < audio_frames + n) cap *= 2;
            int16_t *grown = realloc(audio, cap * 4);
            if (!grown) return;
            audio = grown, audio_cap = cap;
        }
        memcpy(audio + 2 * audio_frames, buf, n * 4);
        for (size_t i = 0; i < 2 * n; i++) {
            const int v = buf[i] < 0 ? -buf[i] : buf[i];
            if (v > audio_peak) audio_peak = v;
        }
        audio_frames += n;
        if (n < 4096) break;
    }
}
static void put32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static void save_wav(const char *path, uint32_t rate) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    uint8_t h[44] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 2, 0};
    const uint32_t bytes = (uint32_t)(audio_frames * 4);
    put32(h + 4, 36 + bytes);
    put32(h + 24, rate);
    put32(h + 28, rate * 4);
    h[32] = 4, h[34] = 16;
    memcpy(h + 36, "data", 4);
    put32(h + 40, bytes);
    fwrite(h, 1, 44, f);
    if (bytes) fwrite(audio, 1, bytes, f);
    fclose(f);
}

/* Inputs at guest times: a key (ADB code, down then up 0.1 s later) or a
 * floppy inserted. */
typedef struct input { double at; int kind; unsigned code; const char *image; int done; } input;
static char typed[4096];             /* typed text not yet given to the modem port */
static size_t typed_n, typed_i;
static int serial_loopback;
static mc_ppc *g_m;
static void serial_out(void *ctx, unsigned port, uint8_t byte) {
    (void)ctx;
    putchar(byte == 13 ? 10 : byte);
    if (!serial_loopback || port != 0) return;
    static size_t matched;
    static const uint8_t request[] = "PBXS\r", reply[] = "RXOK";
    if (byte != request[matched]) matched = 0;
    if (byte == request[matched] && ++matched == sizeof request - 1) {
        const size_t n = mc_ppc_serial_receive(g_m, port, reply, sizeof reply - 1);
        printf("serial loopback: port %u sent %zu, received %zu\n", port, matched, n);
        matched = 0;
    }
}
static input inputs[32];
static unsigned ninputs;
static void net_transmit(void *ctx, const uint8_t *frame, size_t length) { mc_net_send(ctx, frame, length); }
static long io_left;
static uint64_t io_after;
static uint32_t io_low=0x50000000u,io_high=0xFEFFFFFFu;
static uint32_t dump_at, dump_len, watch_at = 1, pc68k_lo, pc68k_hi;
static unsigned pc68k_hits;
static int dump_sp, stop_pc68k, stop_exception;
static uint32_t stop_ppc, stop_ppc_end;
static uint32_t stop_pc68k_at, stop_exception_at;
static uint32_t find_word, find_len;
static int find_a5, cd_first;
static int io_write_only;

/* --until-found: a copy of WORD in RAM that is not the immediate of a 68k
 * move.l #WORD,(An). */
static int found_in_ram(const mc_ppc *m, uint32_t word) {
    const uint8_t want[4] = {(uint8_t)(word >> 24), (uint8_t)(word >> 16), (uint8_t)(word >> 8), (uint8_t)word};
    const uint8_t *p = m->ram + 2, *end = m->ram + m->ram_size;
    /* memchr for the first byte: the scan runs every guest second and must
     * not weigh on what is being timed */
    while (end - p >= 4 && (p = memchr(p, want[0], (size_t)(end - p - 3))) != NULL) {
        if (!((p - m->ram) & 1) && !memcmp(p, want, 4) && ((((uint32_t)p[-2] << 8 | p[-1]) & 0xF1FFu) != 0x20BCu)) return 1;
        ++p;
    }
    return 0;
}
static void observe(void *ctx, uint32_t address, unsigned size, int write, uint32_t value, int fault) {
    (void)ctx;
    if (io_write_only && !write) return;
    if (io_left <= 0 || mc_ppc_now(g_m) < io_after || address < io_low || address > io_high) return;
    /* Each (access, address, place) at most 40 times. */
    static uint32_t hk[1u << 16];
    static uint8_t hn[1u << 16];
    const uint32_t place = g_m->cpu.last_pc >= 0x68000000u && g_m->cpu.last_pc < 0x69000000u ? g_m->cpu.gpr[24] : g_m->cpu.last_pc;
    const uint32_t key = address * 2654435761u ^ place * 40503u ^ (uint32_t)write;
    const unsigned h = (key >> 16 ^ key) & 0xFFFFu;
    if (hk[h] != key) hk[h] = key, hn[h] = 0;
    if (hn[h] >= 40 && !getenv("PDM_NODEDUP")) return;
    hn[h]++;
    io_left--;
    printf("%c %08X %u %08X%s pc=%08X lr=%08X 68k=%08X r3=%08X r4=%08X r31=%08X t=%.6f\n", write ? 'W' : 'R', (unsigned)address, size, (unsigned)value,
           fault ? " FAULT" : "", (unsigned)g_m->cpu.last_pc, (unsigned)g_m->cpu.lr, (unsigned)g_m->cpu.gpr[24], (unsigned)g_m->cpu.gpr[3], (unsigned)g_m->cpu.gpr[4], (unsigned)g_m->cpu.gpr[31], (double)mc_ppc_now(g_m) / MC_C15M_HZ);
}

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: mc_ppc_pdm_boot ROM 6100|7100|8100 SECONDS [options]\n");
        return 2;
    }
    /* MODEL: 6100, 7100, 8100, or a catalogue model's number (7500) or key. */
    const mc_ppc_desc *desc = !strcmp(argv[2], "8100") ? &mc_pdm8100_desc : !strcmp(argv[2], "7100") ? &mc_pdm7100_desc
                              : !strcmp(argv[2], "6100") ? &mc_pdm6100_desc : NULL;
    const char *model_key = NULL;
    static char key[64];
    if (!desc) {
        snprintf(key, sizeof key, "%s%s", (strchr(argv[2], '-') || mc_model_find(argv[2])) ? "" : "power-macintosh-", argv[2]);
        model_key = key;
        desc = mc_ppc_find_board(key);
        if (!desc) {
            fprintf(stderr, "no board serves %s\n", key);
            return 2;
        }
    }
    const double seconds = atof(argv[3]);
    unsigned ram = 16;
    const char *disks[8], *cd = NULL, *floppy = NULL, *shot = NULL, *shots = NULL, *wav = NULL, *raw = NULL;
    const char *audio_input = NULL;
    int accel = 1, startup_sound = 1;
    uint32_t until_word = 0;
    double until_found = -1.0;
    unsigned ndisks = 0, width = 640, height = 480, depth = 8;
    int ro = 0, card = 0, expect_screen = 0, screen = 0, jit = 0, use_net = 0, volumes = 0, serial = 0;
    int disk_bus = MC_BUS_SCSI, cd_bus=MC_BUS_SCSI, control=0;
    double mhz = 0;
    int idle_skip = 1, m68k_accel = 0;
    const char *roms_dir = NULL, *nvram = NULL, *jit_profile = NULL;
    static mc_rom_index index;
    double every = 1.0, until_menu = -1.0;
    uint64_t fine_at = UINT64_MAX;
    unsigned fine_n = 1000000;
    uint32_t watched = 0;
    for (int i = 4; i < argc; i++) {
        if (!strcmp(argv[i], "--ram") && i + 1 < argc) ram = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--disk") && i + 1 < argc && ndisks < 8) disks[ndisks++] = argv[++i];
        else if (!strcmp(argv[i], "--cd") && i + 1 < argc) cd = argv[++i];
        else if (!strcmp(argv[i], "--floppy") && i + 1 < argc) floppy = argv[++i];
        else if (!strcmp(argv[i], "--shot") && i + 1 < argc) shot = argv[++i];
        else if (!strcmp(argv[i], "--shots") && i + 1 < argc) shots = argv[++i];
        else if (!strcmp(argv[i], "--audio-input") && i + 1 < argc) audio_input = argv[++i];
        else if (!strcmp(argv[i], "--wav") && i + 1 < argc) wav = argv[++i];
        else if (!strcmp(argv[i], "--every") && i + 1 < argc) every = atof(argv[++i]);
        else if (!strcmp(argv[i], "--until-menu-bar") && i + 1 < argc) until_menu = atof(argv[++i]);
        else if (!strcmp(argv[i], "--depth") && i + 1 < argc) depth = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--size") && i + 1 < argc) sscanf(argv[++i], "%ux%u", &width, &height);
        else if (!strcmp(argv[i], "--ro")) ro = 1;
        else if (!strcmp(argv[i], "--no-chime")) startup_sound = 0;
        else if (!strcmp(argv[i], "--scsi")) disk_bus = MC_BUS_SCSI;
        else if (!strcmp(argv[i], "--control")) control=1;
        else if (!strcmp(argv[i], "--cd-first")) cd_first=1;
        else if (!strcmp(argv[i], "--cd-ata")) cd_bus=MC_BUS_ATA;
        else if (!strcmp(argv[i], "--cd-scsi")) cd_bus=MC_BUS_SCSI;
        else if (!strcmp(argv[i], "--ata")) disk_bus = MC_BUS_ATA;
        else if (!strcmp(argv[i], "--jit")) jit = 1;
        else if (!strcmp(argv[i], "--no-idle-skip")) idle_skip = 0;
        else if (!strcmp(argv[i], "--m68k-accel")) m68k_accel = 1, jit = 1;
        else if (!strcmp(argv[i], "--m68k-shadow")) m68k_accel = 2, jit = 1;
        else if (!strcmp(argv[i], "--jit-profile") && i + 1 < argc) jit_profile = argv[++i], jit = 1;
        else if (!strcmp(argv[i], "--key") && i + 1 < argc && ninputs + 2 <= 32) {
            unsigned code = 0;
            double at = 0;
            if (sscanf(argv[++i], "%x@%lf", &code, &at) == 2) {
                inputs[ninputs++] = (input){at, 0, code, NULL, 0};
                inputs[ninputs++] = (input){at + 0.1, 1, code, NULL, 0};
            }
        } else if ((!strcmp(argv[i], "--insert-floppy") || !strcmp(argv[i], "--insert-cd")) && i + 2 < argc && ninputs < 32) {
            inputs[ninputs++] = (input){atof(argv[i + 1]), !strcmp(argv[i], "--insert-cd") ? 4 : 2, 0, argv[i + 2], 0};
            i += 2;
        }
        else if (!strcmp(argv[i], "--nvram") && i + 1 < argc) nvram = argv[++i];
        else if (!strcmp(argv[i], "--serial")) serial = 1;
        else if (!strcmp(argv[i], "--serial-loopback")) serial = serial_loopback = 1;
        else if (!strcmp(argv[i], "--mhz") && i + 1 < argc) mhz = atof(argv[++i]);
        else if (!strcmp(argv[i], "--type") && i + 1 < argc && ninputs < 32) {
            char *at = strrchr(argv[++i], '@');
            if (at) {
                *at = 0;
                inputs[ninputs++] = (input){atof(at + 1), 3, 0, argv[i], 0};
            }
        } else if (!strcmp(argv[i], "--hold") && i + 1 < argc && ninputs + 2 <= 32) {
            unsigned code = 0;
            double at = 0;
            if (sscanf(argv[++i], "%x@%lf", &code, &at) == 2) {
                inputs[ninputs++] = (input){0, 0, code, NULL, 0};
                inputs[ninputs++] = (input){at, 1, code, NULL, 0};
            }
        }
        else if (!strcmp(argv[i], "--cuda-fw") && i + 1 < argc) roms_dir = argv[++i];
        else if (!strcmp(argv[i], "--dump") && i + 2 < argc) {
            dump_at = (uint32_t)strtoul(argv[++i], NULL, 16);
            dump_len = (uint32_t)strtoul(argv[++i], NULL, 16);
            dump_sp = 0;
        }
        else if (!strcmp(argv[i], "--dump-sp") && i + 1 < argc) {
            dump_sp = 1;
            dump_len = (uint32_t)strtoul(argv[++i], NULL, 16);
        }
        else if (!strcmp(argv[i], "--stop-pc-range") && i + 2 < argc) {
            stop_ppc = (uint32_t)strtoul(argv[++i], NULL, 16);
            stop_ppc_end = (uint32_t)strtoul(argv[++i], NULL, 16);
        }
        else if (!strcmp(argv[i], "--stop-pc") && i + 1 < argc) stop_ppc = (uint32_t)strtoul(argv[++i], NULL, 16);
        else if (!strcmp(argv[i], "--stop-pc68k") && i + 1 < argc) {
            stop_pc68k = 1;
            stop_pc68k_at = (uint32_t)strtoul(argv[++i], NULL, 16);
        }
        else if (!strcmp(argv[i], "--stop-exception") && i + 1 < argc) {
            stop_exception = 1;
            stop_exception_at = (uint32_t)strtoul(argv[++i], NULL, 16);
        }
        else if (!strcmp(argv[i], "--watch") && i + 1 < argc) watch_at = (uint32_t)strtoul(argv[++i], NULL, 16);
        else if (!strcmp(argv[i], "--pc68k") && i + 2 < argc) {
            pc68k_lo = (uint32_t)strtoul(argv[i + 1], NULL, 16);
            pc68k_hi = (uint32_t)strtoul(argv[i + 2], NULL, 16);
            pc68k_hits = pc68k_hits ? pc68k_hits : 4;
            i += 2;
        }
        else if (!strcmp(argv[i], "--pc68k-hits") && i + 1 < argc) pc68k_hits = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--fine") && i + 2 < argc) {
            fine_at = (uint64_t)(atof(argv[i + 1]) * MC_C15M_HZ);
            fine_n = (unsigned)atoi(argv[i + 2]);
            i += 2;
        }
        else if (!strcmp(argv[i], "--net")) use_net = 1;
        else if (!strcmp(argv[i], "--volumes")) volumes = 1;
        else if (!strcmp(argv[i], "--find-a5")) find_a5 = 1;
        else if (!strcmp(argv[i], "--find") && i + 2 < argc) {
            find_word = (uint32_t)strtoul(argv[++i], NULL, 16);
            find_len = (uint32_t)strtoul(argv[++i], NULL, 16);
        }
        else if (!strcmp(argv[i], "--card")) card = 1;
        else if (!strcmp(argv[i], "--accel") && i + 1 < argc) accel = strcmp(argv[++i], "off") != 0;
        else if (!strcmp(argv[i], "--raw") && i + 1 < argc) raw = argv[++i];
        else if (!strcmp(argv[i], "--until-found") && i + 2 < argc) {
            until_word = (uint32_t)strtoul(argv[i + 1], NULL, 16);
            until_found = atof(argv[i + 2]);
            i += 2;
        }
        else if (!strcmp(argv[i], "--expect-screen")) expect_screen = 1;
        else if (!strcmp(argv[i], "--io") && i + 1 < argc) io_left = atol(argv[++i]);
        else if (!strcmp(argv[i], "--io-write-only")) io_write_only=1;
        else if (!strcmp(argv[i], "--io-range") && i+2<argc) {io_low=(uint32_t)strtoul(argv[++i],NULL,16);io_high=(uint32_t)strtoul(argv[++i],NULL,16);}
        else if (!strcmp(argv[i], "--io-after") && i + 1 < argc) io_after = (uint64_t)(atof(argv[++i]) * MC_C15M_HZ);
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) {
        printf("SKIP: %s is absent\n", argv[1]);
        return 77;
    }
    static uint8_t image[4u << 20];
    const size_t size = fread(image, 1, sizeof image, f);
    fclose(f);
    mc_config c;
    char error[256] = "";
    mc_config_defaults(&c);
    mc_config_set_ram_kb(&c, ram << 10);
    if (model_key) {
        const mc_model *model = mc_model_find(model_key);
        if (model) c.model = (uint32_t)mc_model_index(model), c.cpu_hz = model->cpu_hz;
    }
    if (mhz > 0) c.cpu_hz = (uint64_t)(mhz * 1e6);
    c.cdrom_bus=cd_bus;
    if (desc == &mc_pbx1400c_desc || desc == &mc_pbx1400cs_desc) {
        /* The 1400 can have one bay module. A timed floppy insertion
         * inserts media into a module installed before guest boot. */
        c.floppy_off[0] = !floppy;
        for (unsigned i = 0; i < ninputs; i++)
            if (inputs[i].kind == 2) c.floppy_off[0] = 0;
    }
    if(cd_first)c.boot_priority=MC_BOOT_CDROM;
    c.disk_count = ndisks;
    for (unsigned i = 0; i < ndisks; i++) c.disks[i].bus = disk_bus;
    c.width = width;
    c.height = height;
    c.depth = depth;
    if (card) c.video_card = MC_VIDEO_CARD_LIVADIX;
    c.video_acceleration = accel;
    c.jit = jit;
    c.startup_sound = startup_sound;
    if (nvram) { snprintf(c.nvram, sizeof c.nvram, "%s", nvram); c.nvram_private=1; }
    if (roms_dir) {
        char folder[1024];
        snprintf(folder, sizeof folder, "%s/", roms_dir);
        const char *folders[1] = {folder};
        mc_rom_index_scan(&index, folders, 1);
        c.roms = &index;
        c.controller = MC_CONTROLLER_FIRMWARE;
    }
    mc_ppc *m = mc_ppc_create(desc, &c, image, size, error, sizeof error);
    if (!m) {
        printf("FAIL: %s\n", error);
        return 1;
    }
    printf("configuration: RAM %u MB, disk bus %s\n",(unsigned)(m->ram_size >> 20),disk_bus==MC_BUS_ATA?"ATA":"SCSI");
    m->poll_disabled = !idle_skip;
    /* The board API takes Macintosh-epoch local time, as the app does. */
    const time_t host_now=time(NULL);
    struct tm host_local=*localtime(&host_now),host_utc=*gmtime(&host_now);
    host_utc.tm_isdst=host_local.tm_isdst;
    mc_ppc_set_clock(m,(int64_t)host_now+(int64_t)difftime(mktime(&host_local),mktime(&host_utc))+2082844800ll);
    g_m = m;
    ppc_jit_idle_enable(m->jit, idle_skip);
    if (m68k_accel && !mc_ppc_m68k_accel(m, 1)) { fprintf(stderr, "no 68k accelerator\n"); return 2; }
    if (m68k_accel == 2) mc_m68k_accel_shadow(m->accel, 1);
    if (jit_profile) ppc_jit_profile_enable(m->jit);
    if (serial) {
        static const mc_serial_host sh = {NULL, serial_out, NULL};
        mc_ppc_set_serial(m, &sh);
    }
    FILE *audio_file = NULL;
    if (audio_input) {
        audio_file = fopen(audio_input, "rb");
        const mc_audio_source source = {audio_file, input_pcm};
        if (!audio_file || !mc_ppc_set_audio_source(m, &source)) {
            fprintf(stderr, "FAIL: cannot connect audio input %s\n", audio_input);
            if (audio_file) fclose(audio_file);
            mc_ppc_destroy(m);
            return 1;
        }
    }
    if (io_left) m->bus.observe = observe;
    for (unsigned i = 0; i < ndisks; i++) {
        mc_disk *d = mc_disk_open(disks[i], !ro, 512, error, sizeof error);
        if (!d || !mc_ppc_insert(m, MC_MEDIA_DISK, i, d, error, sizeof error)) printf("disk %s: %s\n", disks[i], error);
    }
    if (cd) {
        mc_disk *d = mc_disk_open(cd, 0, 512, error, sizeof error);
        if (!d || !mc_ppc_insert(m, MC_MEDIA_CDROM, 0, d, error, sizeof error)) printf("cd %s: %s\n", cd, error);
    }
    if (floppy) {
        mc_disk *d = mc_disk_open(floppy, !ro, 512, error, sizeof error);
        if (!d || !mc_ppc_insert(m, MC_MEDIA_FLOPPY, 0, d, error, sizeof error)) printf("floppy %s: %s\n", floppy, error);
    }
    mc_net *net = NULL;
    if (use_net) {
        net = mc_net_open_config(&c, error, sizeof error);
        if (!net) {
            printf("FAIL: network: %s\n", error);
            return 1;
        }
        const mc_net_host host = {net, net_transmit};
        mc_ppc_set_net(m, &host);
    }
    uint64_t end = (uint64_t)(seconds * MC_C15M_HZ);
    uint64_t next = (uint64_t)(every * MC_C15M_HZ);
    /* --until-menu-bar S: once a second of guest time, look for the Finder's
     * menu bar; seen, the run ends S guest seconds later (its drawing done). */
    uint64_t menu_next = until_menu >= 0 ? MC_C15M_HZ : UINT64_MAX;
    uint64_t found_next = until_found >= 0 ? MC_C15M_HZ : UINT64_MAX;
    const clock_t t0 = clock();
    mc_run_status st = MC_RUN_OK;
    while (control || (st == MC_RUN_OK && mc_ppc_now(m) < end)) {
        if(control&&(st!=MC_RUN_OK||mc_ppc_now(m)>=end)){
            char command[1200];printf("control ready t=%.6f status=%d\n",(double)mc_ppc_now(m)/MC_C15M_HZ,(int)st);fflush(stdout);
            if(!fgets(command,sizeof command,stdin))break;
            double span;unsigned code, address, words;int down,dx,dy,button;char path[1024];
            if(sscanf(command,"run %lf",&span)==1&&span>0&&st==MC_RUN_OK)end=mc_ppc_now(m)+(uint64_t)(span*MC_C15M_HZ);
            else if(sscanf(command,"peek %x %u",&address,&words)==2){
                printf("control peek %08X:",address);
                for(unsigned k=0;k<words&&k<256;k++){uint32_t v=0;int ok=mc_ppc_peek(m,address+4*k,4,&v);printf(ok?" %08X":" ????????",v);}
                printf("\n");
            }
            else if(sscanf(command,"poke %x %x",&address,&words)==2)printf("control poke %08X %d\n",address,mc_ppc_poke(m,address,4,words));
            else if(sscanf(command,"type %1023[^\r\n]",path)==1){
                if(typed_i==typed_n)typed_i=typed_n=0;
                for(const char *q=path;*q&&typed_n<sizeof typed;q++){
                    if(q[0]==92&&q[1]=='r')typed[typed_n++]=13,q++;
                    else typed[typed_n++]=*q;
                }
                printf("control type %zu pending\n",typed_n-typed_i);
            }
            else if(!strncmp(command,"state",5)){
                printf("control state pc=%08X lr=%08X msr=%08X dec=%08X tb=%016llX irq=%d dec_pending=%d srr=%08X/%08X dar=%08X dsisr=%08X exceptions=%u\n",
                    m->cpu.pc,m->cpu.lr,m->cpu.msr,m->cpu.dec,(unsigned long long)m->cpu.time_base,
                    m->cpu.external_irq,m->cpu.decrementer_pending,m->cpu.srr0,m->cpu.srr1,m->cpu.dar,m->cpu.dsisr,m->cpu.exceptions);
                printf("control regs");
                for(unsigned k=0;k<32;k++)printf(" r%u=%08X",k,m->cpu.gpr[k]);
                printf(" ctr=%08X cr=%08X\n",m->cpu.ctr,m->cpu.cr);
            }
            else if(sscanf(command,"ram %1023[^\r\n]",path)==1){
                FILE *ram_file=fopen(path,"wb");size_t n=ram_file?fwrite(m->ram,1,m->ram_size,ram_file):0;if(ram_file)fclose(ram_file);
                printf("control ram %zu\n",n);
            }
            else if (!strncmp(command, "video", 5)) {
                mc_framebuffer frame;
                if (mc_ppc_framebuffer(m, &frame))
                    printf("control video %ux%u depth %u stride %u\n",
                           frame.width, frame.height, frame.depth, frame.row_bytes);
                else puts("control video unavailable");
            }
            else if(sscanf(command,"shot %1023[^\r\n]",path)==1)printf("control shot %s %d\n",path,shoot(m,path));
            else if(sscanf(command,"key %x %d",&code,&down)==2)printf("control key %d\n",mc_ppc_key(m,(uint8_t)code,down));
            else if(sscanf(command,"mouse %d %d %d",&dx,&dy,&button)==3)printf("control mouse %d\n",mc_ppc_mouse(m,dx,dy,button));
            else if (sscanf(command, "mouse-to %d %d", &dx, &dy) == 2 && dx >= 0 && dx <= 32767 && dy >= 0 && dy <= 32767) {
                /* The counts a pixel takes are measured from each step (a
                 * device with acceleration, the Applejack's trackball, moves
                 * the pointer further than a mouse does). */
                int reached = 0;
                double per_x = 0.8, per_y = 0.8;
                for (unsigned i = 0; i < 12 && st == MC_RUN_OK; i++) {
                    uint32_t mouse, after;
                    if (!mc_ppc_peek(m, 0x830u, 4, &mouse)) break; /* QuickDraw Mouse point */
                    const int x = dx - (int)(int16_t)mouse, y = dy - (int)(int16_t)(mouse >> 16);
                    if (abs(x) <= 2 && abs(y) <= 2) { reached = 1; break; }
                    int sx = (int)(x * per_x), sy = (int)(y * per_y);
                    if (!sx && x) sx = x > 0 ? 1 : -1;
                    if (!sy && y) sy = y > 0 ? 1 : -1;
                    if (!mc_ppc_mouse(m, sx, sy, 0)) break;
                    st = mc_ppc_run(m, UINT64_MAX, mc_ppc_now(m) + MC_C15M_HZ / 3u);
                    if (!mc_ppc_peek(m, 0x830u, 4, &after)) break;
                    const int mx = (int)(int16_t)after - (int)(int16_t)mouse, my = (int)(int16_t)(after >> 16) - (int)(int16_t)(mouse >> 16);
                    if (sx && mx * sx > 0 && abs(sx) >= 4) per_x = (double)sx / mx;
                    if (sy && my * sy > 0 && abs(sy) >= 4) per_y = (double)sy / my;
                    if (per_x > 0.8) per_x = 0.8;
                    if (per_y > 0.8) per_y = 0.8;
                }
                printf("control mouse-to %d\n", reached);
            }
            else if(sscanf(command,"cd %1023[^\r\n]",path)==1){mc_disk *d=mc_disk_open(path,0,512,error,sizeof error);printf("control cd %d %s\n",d&&mc_ppc_insert(m,MC_MEDIA_CDROM,0,d,error,sizeof error),error);}
            else if(sscanf(command,"floppy %1023[^\r\n]",path)==1){mc_disk *d=mc_disk_open(path,!ro,512,error,sizeof error);printf("control floppy %d %s\n",d&&mc_ppc_insert(m,MC_MEDIA_FLOPPY,0,d,error,sizeof error),error);}
            else if(!strncmp(command,"quit",4))break;
            else printf("control bad command\n");
            continue;
        }
        st = mc_ppc_run(m, mc_ppc_now(m) >= fine_at ? fine_n : net ? 100000 : 1000000, 0);
        if (stop_ppc && m->cpu.pc >= stop_ppc && m->cpu.pc < (stop_ppc_end ? stop_ppc_end : stop_ppc + 4u)) {
            printf("stop: PPC PC %08X\n", (unsigned)m->cpu.pc);
            break;
        }
        if (stop_pc68k && m->cpu.gpr[24] == stop_pc68k_at) {
            printf("stop: 68k PC %08X\n", (unsigned)stop_pc68k_at);
            break;
        }
        if (stop_exception && (fine_at == UINT64_MAX || mc_ppc_now(m) >= fine_at) &&
            m->cpu.last_exception == stop_exception_at &&
            (m->cpu.pc == stop_exception_at || m->cpu.pc == (0xFFF00000u | stop_exception_at))) {
            printf("stop: exception %04X srr0=%08X srr1=%08X dar=%08X dsisr=%08X\n", (unsigned)stop_exception_at,
                   (unsigned)m->cpu.srr0, (unsigned)m->cpu.srr1, (unsigned)m->cpu.dar, (unsigned)m->cpu.dsisr);
            break;
        }
        if (net) {
            static uint8_t frame[1600];
            size_t length;
            while (mc_net_next(net, frame, &length) && mc_ppc_net_receive(m, frame, length)) mc_net_pop(net);
        }
        if (pc68k_hits && m->cpu.gpr[24] >= pc68k_lo && m->cpu.gpr[24] < pc68k_hi) {
            pc68k_hits--;
            printf("68k %08X at t=%.6fs pc=%08X lr=%08X; d0-d7 a0-a7:", (unsigned)m->cpu.gpr[24], (double)mc_ppc_now(m) / MC_C15M_HZ,
                   (unsigned)m->cpu.pc, (unsigned)m->cpu.lr);
            for (int r = 8; r < 24; r++) printf(" %08X", (unsigned)m->cpu.gpr[r]);
            printf("\n  stack:");
            for (uint32_t k = 0; k < 16; k++) {
                uint32_t w = 0;
                mc_ppc_peek(m, m->cpu.gpr[23] + 4 * k, 4, &w);
                printf(" %08X", (unsigned)w);
            }
            printf("\n");
        }
        if (!(watch_at & 1)) {
            uint32_t w = 0;
            mc_ppc_peek(m, watch_at, 2, &w);
            if (w != watched)
                printf("watch %08X: %04X -> %04X at t=%.6fs pc=%08X lr=%08X 68k=%08X\n", (unsigned)watch_at, (unsigned)watched, (unsigned)w,
                       (double)mc_ppc_now(m) / MC_C15M_HZ, (unsigned)m->cpu.pc, (unsigned)m->cpu.lr, (unsigned)m->cpu.gpr[24]);
            watched = w;
        }
        if (wav) pull_audio(m);
        if (typed_i < typed_n && mc_ppc_serial_receive(m, 0, (const uint8_t *)typed + typed_i, 1)) typed_i++;
        for (unsigned k = 0; k < ninputs; k++) {
            input *in = &inputs[k];
            if (in->done || mc_ppc_now(m) < (uint64_t)(in->at * MC_C15M_HZ)) continue;
            in->done = 1;
            if (in->kind < 2) mc_ppc_key(m, (uint8_t)in->code, in->kind == 0);
            else if (in->kind == 3) {
                char text[256];
                size_t n = 0;
                for (const char *q = in->image; *q && n < sizeof text; q++) {
                    if (q[0] == 92 && q[1] == 'r') text[n++] = 13, q++;
                    else text[n++] = *q;
                }
                if (typed_n + n <= sizeof typed) memcpy(typed + typed_n, text, n), typed_n += n;
            } else {
                mc_disk *d = mc_disk_open(in->image, in->kind == 4 ? 0 : !ro, 512, error, sizeof error);
                if (!d || !mc_ppc_insert(m, in->kind == 4 ? MC_MEDIA_CDROM : MC_MEDIA_FLOPPY, 0, d, error, sizeof error)) printf("media %s: %s\n", in->image, error);
                else printf("t=%.2fs %s %s inserted\n", (double)mc_ppc_now(m) / MC_C15M_HZ, in->kind == 4 ? "CD" : "floppy", in->image);
            }
        }
        if (mc_ppc_now(m) >= menu_next) {
            if (menu_bar(m)) {
                const uint64_t stop = mc_ppc_now(m) + (uint64_t)(until_menu * MC_C15M_HZ);
                if (stop < end) end = stop;
                printf("menu bar at t=%.2fs host=%.3fs\n", (double)mc_ppc_now(m) / MC_C15M_HZ, (double)(clock()-t0)/CLOCKS_PER_SEC);
                menu_next = UINT64_MAX;
            } else
                menu_next += MC_C15M_HZ;
        }
        if (mc_ppc_now(m) >= found_next) {
            if (found_in_ram(m, until_word)) {
                const uint64_t stop = mc_ppc_now(m) + (uint64_t)(until_found * MC_C15M_HZ);
                if (stop < end) end = stop;
                printf("found %08X at t=%.2fs\n", (unsigned)until_word, (double)mc_ppc_now(m) / MC_C15M_HZ);
                found_next = UINT64_MAX;
            } else
                found_next += MC_C15M_HZ;
        }
        if (mc_ppc_now(m) >= next) {
            uint32_t w = 0;
            mc_ppc_peek(m, 0x0824, 4, &w);
            printf("t=%.2fs instr=%llu pc=%08X lr=%08X 68k=%08X scrnbase=%08X host=%.1fs\n", (double)mc_ppc_now(m) / MC_C15M_HZ,
                   (unsigned long long)m->cpu.instructions, (unsigned)m->cpu.pc, (unsigned)m->cpu.lr, (unsigned)m->cpu.gpr[24],
                   (unsigned)w, (double)(clock() - t0) / CLOCKS_PER_SEC);
            if (shots) {
                char path[1024];
                snprintf(path, sizeof path, "%s%03u.bmp", shots, (unsigned)((double)mc_ppc_now(m) / MC_C15M_HZ + 0.5));
                shoot(m, path);
            }
            fflush(stdout);
            next += (uint64_t)(every * MC_C15M_HZ);
        }
    }
    if (st == MC_RUN_HALTED) printf("powered off\n");
    printf("end: status %d %s at t=%.2fs instr=%llu pc=%08X host %.1fs\n", (int)st, st == MC_RUN_ERROR ? mc_ppc_error(m) : "",
           (double)mc_ppc_now(m) / MC_C15M_HZ, (unsigned long long)m->cpu.instructions, (unsigned)m->cpu.pc,
           (double)(clock() - t0) / CLOCKS_PER_SEC);
    {
        /* The code around the PC, as words (for a disassembler). */
        const uint32_t pc = m->cpu.pc & ~3u;
        printf("code at %08X:", (unsigned)(pc - 0x40));
        for (uint32_t a = pc - 0x40; a < pc + 0x40; a += 4) {
            uint32_t w = 0;
            mc_ppc_peek(m, a, 4, &w);
            printf(" %08X", (unsigned)w);
        }
        if (dump_len) {
            if (dump_sp) dump_at = m->cpu.gpr[1];
            printf("\ndump at %08X:", (unsigned)dump_at);
            for (uint32_t a = dump_at; a < dump_at + dump_len; a += 4) {
                uint32_t w = 0;
                mc_ppc_peek(m, a, 4, &w);
                printf(" %08X", (unsigned)w);
            }
        }
        printf("\nregs:");
        for (int r = 0; r < 32; r++) printf(" r%d=%08X", r, (unsigned)m->cpu.gpr[r]);
        printf(" ctr=%08X msr=%08X\n", (unsigned)m->cpu.ctr, (unsigned)m->cpu.msr);
    }
    {
        mc_framebuffer fb;
        memset(&fb, 0, sizeof fb);
        screen = mc_ppc_framebuffer(m, &fb);
        if (screen) printf("video: %ux%u, %u bits, %u bytes per row\n",fb.width,fb.height,fb.native_depth ? fb.native_depth : fb.depth,fb.native_row_bytes ? fb.native_row_bytes : fb.row_bytes);
        if (screen && raw && fb.pixels) {
            FILE *rf = fopen(raw, "wb");
            const size_t row = ((size_t)fb.width * fb.depth + 7) / 8;
            for (uint32_t y = 0; rf && y < fb.height; y++) fwrite(fb.pixels + (size_t)y * fb.row_bytes, 1, row, rf);
            if (rf) fclose(rf), printf("raw screen saved\n");
        }
        printf("blitter: %u operations\n", (unsigned)mc_ppc_card_blits(m));
    }
    if (shot) {
        if (shoot(m, shot)) printf("screen saved\n");
        else printf("no screen\n");
    }
    {
        uint32_t box = 0;
        if (mc_ppc_peek(m, 0x0CB0, 4, &box)) printf("boxflag: %u\n", (unsigned)(box & 0xFFu));
        if (mc_ppc_peek(m, 0x015A, 2, &box)) printf("system-version: %04X\n", (unsigned)box);
    }
    if (wav) {
        save_wav(wav, mc_ppc_audio_rate(m));
        printf("audio peak %d\n", audio_peak);
    }
    if (volumes && st == MC_RUN_OK) {
        /* The mounted volumes: the VCB queue (VCBQHdr's head at $358; a
         * VCB's link at +0, its name, a Str27, at +44; Inside Macintosh
         * IV, "The File Manager"), read through the 68k world's
         * translation: while another nanokernel task runs, a little more
         * is run first. */
        for (int tries = 0; tries < 200; tries++) {
            char names[8][32];
            unsigned n = 0;
            int ok = 1;
            uint32_t vcb = 0;
            ok = mc_ppc_peek(m, 0x358, 4, &vcb);
            for (unsigned k = 0; ok && vcb && k < 8; k++) {
                uint32_t len = 0;
                ok = !(vcb & 1) && mc_ppc_peek(m, vcb + 44, 1, &len) && len > 0 && len < 28;
                for (uint32_t i = 0; ok && i < len; i++) {
                    uint32_t ch = 0;
                    ok = mc_ppc_peek(m, vcb + 45 + i, 1, &ch) && ch >= 0x20;
                    names[n][i] = (char)ch;
                }
                if (ok) names[n++][len] = 0;
                ok = ok && mc_ppc_peek(m, vcb, 4, &vcb);
            }
            if (ok && n) {
                for (unsigned k = 0; k < n; k++) printf("volume: %s\n", names[k]);
                break;
            }
            mc_ppc_run(m, 100000, 0);
        }
    }
    if (find_len && find_a5 && st == MC_RUN_OK) {
        /* The ordinary 68k network probe keeps its result address in A5
         * (the ROM emulator's r21). With VM, adjacent logical pages can
         * occupy unrelated physical pages: read each byte through the MMU.
         * A brief bounded retry lets an interrupted 68k task resume. */
        uint8_t *result=malloc(find_len);
        if(result) {
            for(unsigned tries=0;tries<200;++tries) {
                uint32_t address=m->cpu.gpr[21],tag=0,before=0;
                if(mc_ppc_peek(m,address,4,&tag)&&tag==find_word) {
                    int ok=1;
                    for(uint32_t k=0;ok&&k<find_len;++k){uint32_t v=0;ok=mc_ppc_peek(m,address+k,1,&v);result[k]=(uint8_t)v;}
                    if(ok){mc_ppc_peek(m,address-2,2,&before);printf("found %08X after %04X:",(unsigned)address,(unsigned)before);
                        for(uint32_t k=0;k<find_len;++k)printf(" %02X",result[k]);
                        printf("\n");break;}
                }
                if(mc_ppc_run(m,10000,0)!=MC_RUN_OK)break;
            }
            free(result);
        }
    }
    if (find_len) {
        /* Every 2-byte-aligned copy of the 4 bytes in RAM, with what follows. */
        for (uint32_t a = 0; a + 4 + find_len <= m->ram_size; a += 2) {
            const uint8_t *r = m->ram + a;
            if (((uint32_t)r[0] << 24 | (uint32_t)r[1] << 16 | (uint32_t)r[2] << 8 | r[3]) != find_word) continue;
            const uint32_t before = a >= 2 ? (uint32_t)r[-2] << 8 | r[-1] : 0;
            printf("found %08X after %04X:", (unsigned)a, (unsigned)before);
            for (uint32_t k = 0; k < find_len; k++) printf(" %02X", r[k]);
            printf("\n");
        }
    }
    if (net) {
        mc_nat_stats s;
        mc_net_stats(net, &s);
        printf("network: %u DHCP offers, %u DHCP acks, %u BOOTP replies, %u ARP replies, %u DNS queries, %u TCP connections "
               "(%u refused, %u closed), %llu bytes out, %llu bytes in, %u retransmissions\n", s.dhcp_offers, s.dhcp_acks,
               s.bootp_replies, s.arp_replies, s.dns_queries, s.tcp_opened, s.tcp_refused, s.tcp_closed,
               (unsigned long long)s.tcp_bytes_out, (unsigned long long)s.tcp_bytes_in, s.tcp_retransmits);
        mc_ppc_set_net(m, NULL);
        mc_net_close(net);
    }
    if (m->jit) {
        ppc_jit_stats stats;
        ppc_jit_get_stats(m->jit, &stats);
        if (m->accel) {
            mc_m68k_accel_stats as;
            mc_m68k_accel_get_stats(m->accel, &as);
            printf("68k accelerator: %llu instructions (%llu translated, %llu blocks) in %llu runs, %llu entries left to the emulator\n",
                   (unsigned long long)as.instructions, (unsigned long long)as.translated, (unsigned long long)as.blocks, (unsigned long long)as.runs, (unsigned long long)as.declined);
            if (as.checked) printf("68k shadow: %llu instructions compared with Apple's emulator, %llu differ\n",
                                   (unsigned long long)as.checked, (unsigned long long)as.differed);
        }
        printf("jit: %llu idle steps folded, %llu probes, %llu helpers, %llu memory slow paths\n",
               (unsigned long long)stats.idle_steps, (unsigned long long)stats.idle_probes,
               (unsigned long long)stats.helpers, (unsigned long long)stats.memory_slow);
        printf("jit register cache: %llu blocks, %llu operands\n",
               (unsigned long long)stats.cached_gpr_blocks, (unsigned long long)stats.cached_gpr_uses);
    }
    if (audio_file) {
        mc_ppc_set_audio_source(m, NULL);
        fclose(audio_file);
    }
    if (jit_profile && !ppc_jit_profile_write(m->jit, jit_profile)) {
        fprintf(stderr, "FAIL: cannot write JIT profile %s\n", jit_profile);
        mc_ppc_destroy(m);
        return 1;
    }
    if (m->poll_steps) printf("device polls: %llu stationary instructions advanced\n", (unsigned long long)m->poll_steps);
    mc_ppc_destroy(m);
    if (expect_screen && !screen) printf("FAIL: no screen\n");
    return st == MC_RUN_ERROR || (expect_screen && !screen && st != MC_RUN_HALTED) ? 1 : 0;
}
