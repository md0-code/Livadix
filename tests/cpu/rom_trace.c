/* mc_rom_trace ROM [options]: runs a 68040 Macintosh ROM (or with --68000 a
 * compact Macintosh's) from reset in a bare memory map and reports what it
 * touches outside ROM and RAM.
 *
 *   --ram MB        RAM at address 0 after the overlay (default 8)
 *   --max N         instruction limit (default 1,000,000)
 *   --trace FILE    one line per instruction (PC, words, mnemonic, D0-D7/A0-A7)
 *   --trace-max N   lines written to the trace (default 100,000)
 *   --continue K    do not stop at the first unmapped access: answer reads
 *                   with 0, drop writes, and list the first K distinct
 *                   addresses touched (default: stop at the first)
 *   --io-read BYTE  the value reads outside ROM and RAM answer, repeated in
 *                   every byte of the access (default 0)
 *   --check         exit 0 only if the run ends as it should: at an I/O
 *                   access (default mode) or at the instruction limit
 *                   (--continue), never at an opcode of a later step or a
 *                   halt; exit 77 when the ROM file is absent (ctest skip)
 *   --68000         the 68000 model on the compact Macintosh's 24-bit map
 *                   (Guide to the Macintosh Family Hardware, 2nd ed., the
 *                   Macintosh Plus memory map): RAM from 0 (at most 4 MB),
 *                   the ROM at $400000 through $4FFFFF and at 0 from reset
 *                   until the first access to its own range; everything
 *                   else (the SCSI, SCC, IWM and VIA ranges) unmapped
 *
 * The map is the Quadra 700's as its Developer Note describes it: the ROM at
 * $40000000 (mirrored through $4FFFFFFF) and, from reset, also at address 0
 * (the overlay) until the first access to the ROM's own address range; RAM
 * at 0 afterwards. Everything else (I/O at $50000000, NuBus space, RAM
 * beyond the size given) is "unmapped" here: no device is emulated.
 * Headless; writes only the trace file it is given. */
#include "m68k.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ROM_BASE 0x40000000u
#define IO_BASE 0x50000000u

typedef struct access {
    uint32_t address, pc, value;
    uint8_t size, write;
    uint64_t when;
} access;

typedef struct harness {
    uint32_t rom_base, rom_end;     /* the ROM's own range; I/O from IO_BASE (68040) or anything else (68000) */
    int m68000;
    uint8_t *rom, *ram;
    uint32_t rom_size, ram_size;
    uint8_t **map;
    int overlay;
    int stop_at_first;
    unsigned want, logged;
    access *log;
    m68k_cpu *cpu;
    int stopped;
    uint32_t io_read;   /* what reads outside ROM and RAM answer (the byte repeated) */
} harness;

static void map_rom_at(harness *h, uint32_t base, uint32_t span) {
    for (uint32_t a = 0; a < span; a += M68K_PAGE_SIZE) h->map[(base + a) >> M68K_PAGE_SHIFT] = h->rom + (a % h->rom_size);
}

static void overlay_off(harness *h) {
    h->overlay = 0;
    for (uint32_t a = 0; a < h->rom_base; a += M68K_PAGE_SIZE) h->map[a >> M68K_PAGE_SHIFT] = NULL;
    for (uint32_t a = 0; a < h->ram_size; a += M68K_PAGE_SIZE) h->map[a >> M68K_PAGE_SHIFT] = h->ram + a;
    map_rom_at(h, h->rom_base, h->rom_end - h->rom_base);
}

static const char *region(const harness *h, uint32_t a) {
    if (h->m68000) {
        if (a < h->ram_size) return "RAM";
        if (a < h->rom_base) return "RAM space beyond the RAM fitted";
        return a < h->rom_end ? "ROM" : "I/O";
    }
    if (a >= IO_BASE && a < 0x60000000u) return "I/O";
    if (a < 0x40000000u) return a < h->ram_size ? "RAM" : "RAM space beyond the RAM fitted";
    if (a >= 0x60000000u) return "NuBus space";
    return "ROM";
}

static void note(harness *h, uint32_t a, unsigned size, int write, uint32_t value) {
    for (unsigned i = 0; i < h->logged; i++)
        if (h->log[i].address == a && h->log[i].write == write) return;
    if (h->logged < h->want) {
        access *x = &h->log[h->logged++];
        x->address = a;
        x->pc = h->cpu->ipc;
        x->size = (uint8_t)size;
        x->write = (uint8_t)write;
        x->value = value;
        x->when = h->cpu->instructions;
    }
    if (h->stop_at_first || h->logged >= h->want) h->stopped = 1;
}

static int h_read(void *ctx, uint32_t a, unsigned size, unsigned fc, uint32_t *v) {
    harness *h = ctx;
    (void)fc;
    if (a >= h->rom_base && a < h->rom_end) {
        if (h->overlay) overlay_off(h);
        uint32_t r = 0;
        for (unsigned i = 0; i < size; i++) r = (r << 8) | h->rom[(a + i - h->rom_base) % h->rom_size];
        *v = r;
        return 0;
    }
    if (h->overlay && a < h->rom_base) {
        uint32_t r = 0;
        for (unsigned i = 0; i < size; i++) r = (r << 8) | h->rom[(a + i) % h->rom_size];
        *v = r;
        return 0;
    }
    if (a < h->ram_size && a + size <= h->ram_size) {
        uint32_t r = 0;
        for (unsigned i = 0; i < size; i++) r = (r << 8) | h->ram[a + i];
        *v = r;
        return 0;
    }
    note(h, a, size, 0, 0);
    *v = h->io_read & (size == 1 ? 0xFFu : size == 2 ? 0xFFFFu : 0xFFFFFFFFu);
    return 0;
}

static int h_write(void *ctx, uint32_t a, unsigned size, unsigned fc, uint32_t v) {
    harness *h = ctx;
    (void)fc;
    if (a >= h->rom_base && a < h->rom_end) {
        if (h->overlay) overlay_off(h);
        return 0;   /* ROM writes are ignored */
    }
    if (!h->overlay && a < h->ram_size && a + size <= h->ram_size) {
        for (unsigned i = 0; i < size; i++) h->ram[a + i] = (uint8_t)(v >> (8u * (size - 1u - i)));
        return 0;
    }
    if (h->overlay && a < h->rom_base) return 0;
    note(h, a, size, 1, v);
    return 0;
}

static uint8_t *load(const char *path, uint32_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *data = malloc((size_t)n);
    if (fread(data, 1, (size_t)n, f) != (size_t)n) {
        free(data);
        data = NULL;
    }
    fclose(f);
    *size = (uint32_t)n;
    return data;
}

/* A word as the CPU would fetch it, without side effects. */
static uint32_t peek16(const harness *h, uint32_t a) {
    const uint8_t *p = h->map[a >> M68K_PAGE_SHIFT];
    if (p) return (uint32_t)p[a & (M68K_PAGE_SIZE - 1)] << 8 | p[(a + 1) & (M68K_PAGE_SIZE - 1)];
    if (a >= h->rom_base && a < h->rom_end) return (uint32_t)h->rom[(a - h->rom_base) % h->rom_size] << 8 | h->rom[(a + 1 - h->rom_base) % h->rom_size];
    return 0;
}

static void trace_line(FILE *t, harness *h, m68k_cpu *cpu) {
    const uint32_t pc = cpu->pc;
    uint32_t words[3];
    for (unsigned i = 0; i < 3; i++) words[i] = peek16(h, pc + 2 * i);
    fprintf(t, "%08X %04X %04X %04X %-14s", (unsigned)pc, (unsigned)words[0], (unsigned)words[1], (unsigned)words[2],
            m68k_decode(cpu->model, (uint16_t)words[0]).name);
    for (unsigned i = 0; i < 16; i++) fprintf(t, " %08X", (unsigned)cpu->r[i]);
    fprintf(t, " SR %04X\n", m68k_sr(cpu));
}

int main(int argc, char **argv) {
    const char *rom_path = NULL, *trace_path = NULL;
    unsigned ram_mb = 0, cont = 0;
    int check = 0, m68000 = 0;
    uint32_t io_read = 0;
    uint64_t max = 1000000, trace_max = 100000;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--ram") && i + 1 < argc) ram_mb = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--max") && i + 1 < argc) max = strtoull(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--trace") && i + 1 < argc) trace_path = argv[++i];
        else if (!strcmp(argv[i], "--trace-max") && i + 1 < argc) trace_max = strtoull(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--continue") && i + 1 < argc) cont = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--check")) check = 1;
        else if (!strcmp(argv[i], "--68000")) m68000 = 1;
        else if (!strcmp(argv[i], "--io-read") && i + 1 < argc) io_read = (uint32_t)strtoul(argv[++i], NULL, 0) & 0xFFu;
        else if (!rom_path) rom_path = argv[i];
        else {
            fprintf(stderr, "unknown argument %s\n", argv[i]);
            return 2;
        }
    }
    if (!rom_path) {
        fprintf(stderr, "usage: mc_rom_trace ROM [--68000] [--ram MB] [--max N] [--trace FILE] [--trace-max N] [--continue K]\n");
        return 2;
    }
    harness h;
    memset(&h, 0, sizeof h);
    h.rom = load(rom_path, &h.rom_size);
    if (!h.rom && check) {
        printf("%s: absent; skipped\n", rom_path);
        return 77;
    }
    if (!h.rom || h.rom_size < 8 || (h.rom_size & (h.rom_size - 1))) {
        fprintf(stderr, "%s: cannot read a ROM image\n", rom_path);
        return 2;
    }
    h.m68000 = m68000;
    h.rom_base = m68000 ? 0x400000u : ROM_BASE;
    h.rom_end = m68000 ? 0x500000u : IO_BASE;
    if (!ram_mb) ram_mb = m68000 ? 4 : 8;
    if (m68000 && ram_mb > 4) ram_mb = 4;
    h.ram_size = ram_mb << 20;
    h.io_read = io_read * 0x01010101u;
    h.ram = calloc(h.ram_size, 1);
    h.map = calloc(M68K_PAGE_COUNT, sizeof *h.map);
    h.overlay = 1;
    h.stop_at_first = cont == 0;
    h.want = cont ? cont : 1;
    h.log = calloc(h.want, sizeof *h.log);
    /* Reset: the overlay puts the ROM at 0; its own range stays off the
     * page table until the first access there releases the overlay. */
    map_rom_at(&h, 0, h.rom_base);
    m68k_bus bus;
    memset(&bus, 0, sizeof bus);
    bus.ctx = &h;
    bus.read_map[0] = bus.read_map[1] = h.map;
    bus.write_map[0] = bus.write_map[1] = NULL;
    bus.read = h_read;
    bus.write = h_write;
    m68k_cpu *cpu = calloc(1, sizeof *cpu);
    h.cpu = cpu;
    m68k_init(cpu, m68000 ? M68K_68000 : M68K_68040, &bus);
    m68k_reset(cpu);
    const uint32_t checksum = (uint32_t)h.rom[0] << 24 | (uint32_t)h.rom[1] << 16 | (uint32_t)h.rom[2] << 8 | h.rom[3];
    printf("ROM %s: %u KiB, checksum %08X; reset ISP %08X, PC %08X\n", rom_path, (unsigned)(h.rom_size >> 10),
           (unsigned)checksum, (unsigned)cpu->r[15], (unsigned)cpu->pc);
    FILE *t = trace_path ? fopen(trace_path, "w") : NULL;
    m68k_status st = M68K_OK;
    while (cpu->instructions < max && !h.stopped) {
        if (t && cpu->instructions < trace_max) trace_line(t, &h, cpu);
        const uint64_t before = cpu->instructions;
        st = m68k_run(cpu, 1);
        if (st != M68K_OK || cpu->instructions == before) break;
    }
    if (t) fclose(t);
    printf("%llu instructions; ", (unsigned long long)cpu->instructions);
    if (st == M68K_UNIMPLEMENTED) printf("stopped at %08X: opcode %04X (%s) belongs to a later step\n", (unsigned)cpu->pc,
                                         (unsigned)cpu->unimplemented_op, m68k_decode(cpu->model, (uint16_t)cpu->unimplemented_op).name);
    else if (st == M68K_HALTED) printf("halted (double fault) at %08X\n", (unsigned)cpu->pc);
    else if (st == M68K_STOPPED) printf("STOP at %08X with SR %04X\n", (unsigned)cpu->pc, m68k_sr(cpu));
    else if (h.stopped && h.stop_at_first) printf("stopped at the first access outside ROM and RAM\n");
    else if (h.stopped) printf("stopped after %u distinct accesses outside ROM and RAM\n", h.logged);
    else printf("instruction limit reached\n");
    printf("final PC %08X, SR %04X; overlay %s\n", (unsigned)cpu->pc, m68k_sr(cpu), h.overlay ? "still on" : "released");
    for (unsigned i = 0; i < h.logged; i++) {
        const access *x = &h.log[i];
        printf("  %8llu  PC %08X  %s.%c %08X (%s)", (unsigned long long)x->when, (unsigned)x->pc, x->write ? "write" : "read ",
               x->size == 1 ? 'B' : x->size == 2 ? 'W' : 'L', (unsigned)x->address, region(&h, x->address));
        if (x->write) printf(" = %0*X", 2 * x->size, (unsigned)x->value);
        printf("\n");
    }
    int ok = st == M68K_OK;
    if (check) ok = ok && (cont ? !h.stopped && cpu->instructions >= max
                                : h.stopped && h.logged && !strcmp(region(&h, h.log[0].address), "I/O"));
    m68k_free(cpu);
    free(cpu);
    free(h.log);
    free(h.map);
    free(h.ram);
    free(h.rom);
    if (check) return ok ? 0 : 1;
    return st == M68K_HALTED ? 1 : 0;
}
