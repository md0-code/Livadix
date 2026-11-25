/* mc_hw_trace ROM [options]: runs a real machine (src/hw) from reset,
 * headless, and reports what the ROM did: device accesses, the last
 * instructions, exceptions, where it ended.
 *
 *   --model KEY       model (default quadra-700)
 *   --ram MB          emulation.ram_mb (default 8; 2.5 for 2.5 MB)
 *   --mhz N           emulation.cpu_mhz (default the model's clock)
 *   --max N           instructions to run (default 10,000,000)
 *   --io N            log the first N device accesses (default 0)
 *   --io-from N       start logging device accesses at instruction N
 *   --io-filter A M   log only addresses with (address & M) == A
 *   --pcs N           print the last N instruction addresses (default 32)
 *   --step            run one instruction per call (needed by --pcs and
 *                     --stop-pc; implied by them)
 *   --stop-pc ADDR    stop when the PC reaches ADDR
 *   --log-pc ADDR     (stepping) print D0, D1, A0, A1 and the return
 *                     address on the stack whenever the PC reaches ADDR
 *   --watch ADDR      (stepping) report every change of the longword at ADDR
 *   --step-from N     run unstepped to instruction N first (with --pcs, --stop-pc)
 *   --exceptions N    log the first N exceptions (vector, PC)
 *   --nvram FILE      PRAM file (default none)
 *   --clock SECONDS   the host's time at start, seconds since 1904
 *                     (mc_machine_set_clock; default none)
 *   --resolution WxH, --depth D   video settings
 *   --video-card KEY  video.card: auto, builtin or livadix
 *   --accel on|off  video.acceleration (default on)
 *   --controller KEY  emulation.controller: auto, protocol or firmware (the
 *                     firmware from the ROM file's folder, romdb.h)
 *   --key CODE@N, --mouse DX,DY@N  inject input after N instructions
 *                     (CODE in hex, | 0x80 for a release)
 *   --point X,Y@N     from instruction N, move the cursor to X,Y in small
 *                     steps, reading where the System put it (low memory
 *                     Mouse, $830), then go on
 *   --down@N, --up@N  the mouse button
 *   --alerts          print every alert and system error box the
 *                     System puts up ("alert: ...")
 *   --control C,V@N   mc_machine_control(C, V) at instruction N (C: 0 the
 *                     keyswitch, 1 brightness, 2 contrast, 3 volume, 4 the
 *                     interrupt switch, 5 the reset switch: V 1 pressed, 0
 *                     released); prints the result
 *   --shot-at FILE@N  save the screen at instruction N
 *                     (every @N of an input may be @+N: N instructions
 *                     after the Finder came up, with --finder)
 *   --insert-floppy FILE@N, --insert-cdrom FILE@N   insert an image while
 *                     the Macintosh runs, as a drop on the window does
 *   --shot FILE.bmp   save the screen at the end (24-bit BMP)
 *   --disk FILE, --disk-ro FILE   a hard disk image (writable, or read-only)
 *   --disk-ata FILE, --disk-ata-ro FILE   the same on the ATA bus
 *                     (storage.disks bus: ata)
 *   --cdrom FILE, --floppy FILE   a CD-ROM image; a floppy image (writable)
 *   --cdrom-ata       the CD-ROM drive on the ATA bus (storage.cdrom bus:
 *                     ata; --cdrom and --insert-cdrom then go there)
 *   --fpu-full        emulation.fpu_full
 *   --pmmu            emulation.pmmu (a 68851 in the Macintosh II)
 *   --charger-start   emulation.charger_start
 *   --boot-cdrom      storage.boot_priority: cdrom
 *   --no-chime        emulation.startup_sound: false
 *   --trap WORD       log every A-line trap WORD (hex, its flag bits 9-10
 *                     ignored) with D0, D1, A0 and the PC (the trap word read
 *                     physically at the PC); Control and Status calls with
 *                     their refNum and csCode; FFFF logs every trap
 *   --peek ADDR N     at the end, print N longwords of logical memory from ADDR
 *   --volumes         at the end, list the mounted volumes (the File
 *                     Manager's VCB queue: VCBQHdr at $356, vcbVN at +44,
 *                     Inside Macintosh IV, "The File Manager")
 *   --finder N        stop N instructions after the Finder became the
 *                     current application (low memory CurApName, $910),
 *                     and report when that was (instructions, guest
 *                     time, host time since the start)
 *   --run-code FILE   at the end, copy a position-independent 68k program
 *                     64 KiB below the supervisor stack, run it in
 *                     supervisor mode with interrupts masked until it
 *                     reaches its `bra.s *', and print its last 512 bytes
 *                     (its results)
 *   --net             plug the Ethernet into the segment with the NAT
 *                     (src/net; LIVADIX_NET_LOG prints its events), served
 *                     between run slices; the NAT's counters at the end
 *   --share NAME=FOLDER  with --net, share a folder through the file server
 *                     (sharing.folders; repeatable; NAME=FOLDER:ro for a
 *                     read-only one); --share-appledouble keeps Macintosh
 *                     metadata in AppleDouble files (sharing.metadata);
 *                     --share-password PW (sharing.password)
 *   --live-code FILE@N  at instruction N (@+N: after the Finder came up),
 *                     once the CPU runs at interrupt level 0 in supervisor
 *                     mode with the Finder current, about to call its
 *                     event loop's trap (_WaitNextEvent, _GetNextEvent or
 *                     _EventAvail), copy a position-
 *                     independent 68k program below the Finder's stack
 *                     (above ApplLimit), run it with interrupts enabled
 *                     until it reaches its `bra.s *', then restore every
 *                     register (as after a long interrupt) and print its
 *                     last 512 bytes (its results). From then on guest time
 *                     is paced to wall time, so the program's timeouts and
 *                     the network's agree
 *   --live-then N     stop N instructions after the live program ended
 *   --live-stack BYTES  the live program's stack (default 8 KiB)
 *                     A live program may move itself (where it sits can be
 *                     handed out by the Memory Manager meanwhile): it then
 *                     writes its copy's address at $FC (vector 63, unused;
 *                     cleared at its start and restored at its end), and its
 *                     results and its `bra.s *' are the copy's
 *   --live-arg VALUE  a long written at offset 4, 8, ... of the live
 *                     program (repeatable)
 *   --drivers         at the end, list the unit table's drivers
 *   --gestalt SEL     at the end, ask the guest's Gestalt for SEL (four
 *                     characters, repeatable): the CPU runs the ROM's own
 *                     _Gestalt call at ROM offset $C78A with D0 = SEL
 *                     (a 256 KB ROM: the trap from below the stack),
 *                     interrupts masked, and every register is restored
 *   --audio-input FILE  the sound input: 16-bit little-endian stereo
 *                     frames, taken one per input sample whatever the
 *                     guest's rate, the file repeated
 *   --find WORD N     at the end, print N bytes (hex) from each long WORD
 *                     (hex) found in RAM, except where it is the immediate
 *                     of a move.l #WORD,(An) (a probe's results)
 *   --wav FILE.wav    save the sound output (16-bit stereo WAV) and report
 *                     its loudest sample and the first sounding time
 *   --hot N           sample the PC every 997 instructions and print the N
 *                     most frequent (where the guest spends its time)
 *   --interp          the interpreter alone (emulation.jit false)
 *   --no-callout      translated blocks leave before an access that misses
 *                     the page tables instead of calling the slow path
 *   --no-idle-skip    busy-wait loops run iteration by iteration
 *   --checkpoint N    every N instructions, a hash of the registers, the
 *                     FPU, the MMU registers, guest time, RAM and the screen
 *                     (interpreter and translator runs must print the same)
 *   --jit-stats       the translator's counters at the end
 *   --io-count        count device accesses (reported when the Finder comes up)
 *   --dump ADDR LEN   print memory at the end (repeatable; @ADDR: at the
 *                     longword stored at ADDR); ~ADDR: a logical address,
 *                     translated by the MMU as a supervisor data access
 *
 * A run ends early when the Macintosh switches itself off (its last line
 * then says "powered off"). When the processor stops running (a PowerBook
 * asleep) for 30 s of guest time, the next input is given at once (a key
 * wakes it), or, with none left, the run ends ("asleep").
 *
 * Writes nothing but the PRAM file it is given. */
#include "../../src/hw/hw_machine.h"
#include "../../src/core/models.h"
#include "../../src/core/romdb.h"
#include "../../src/core/cpu/m68k_internal.h"
#include "../../src/net/net.h"
#include "../../src/net/sys.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct io_entry {
    uint64_t when;
    uint32_t pc, address, value;
    uint8_t size, write, fault;
} io_entry;

static io_entry *io_log;
static unsigned io_want, io_count;
static uint64_t io_from;
static uint32_t io_addr, io_mask;
static mc_hw *the_hw;

static uint64_t io_reads, io_writes;
static void observe(void *ctx, uint32_t address, unsigned size, int write, uint32_t value, int fault) {
    (void)ctx;
    if (write) io_writes++;
    else io_reads++;
    if (io_count >= io_want || the_hw->cpu->instructions < io_from) return;
    if ((address & io_mask) != io_addr) return;
    io_entry *e = &io_log[io_count++];
    e->when = the_hw->cpu->instructions;
    e->pc = the_hw->cpu->ipc;
    e->address = address;
    e->value = value;
    e->size = (uint8_t)size;
    e->write = (uint8_t)write;
    e->fault = (uint8_t)fault;
}

/* --checkpoint: FNV-1a over the state the guest can see. */
static uint64_t fnv(uint64_t h, const void *data, size_t n) {
    const uint8_t *p = data;
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 0x100000001B3ull;
    return h;
}
static uint64_t state_hash(mc_machine *m, mc_hw *hw) {
    const m68k_cpu *c = hw->cpu;
    uint64_t h = 0xCBF29CE484222325ull;
    uint32_t regs[34];
    memcpy(regs, c->r, 64);
    regs[16] = c->pc;
    regs[17] = m68k_sr(c);
    regs[18] = m68k_get_usp(c);
    regs[19] = m68k_get_isp(c);
    regs[20] = m68k_get_msp(c);
    regs[21] = c->vbr;
    regs[22] = c->cacr;
    regs[23] = c->tc;
    regs[24] = c->urp;
    regs[25] = c->srp;
    regs[26] = c->itt0;
    regs[27] = c->itt1;
    regs[28] = c->dtt0;
    regs[29] = c->dtt1;
    regs[30] = c->mmusr;
    regs[31] = c->fpu.fpcr;
    regs[32] = c->fpu.fpsr;
    regs[33] = c->fpu.fpiar;
    h = fnv(h, regs, sizeof regs);
    for (unsigned i = 0; i < 8; i++) {
        h = fnv(h, &c->fpu.fp[i].m, 8);
        h = fnv(h, &c->fpu.fp[i].se, 2);
    }
    /* The ATCs and their replacement counters (what later translations
     * replace). */
    h = fnv(h, c->mmu.ctr, sizeof c->mmu.ctr);
    h = fnv(h, c->mmu.atc, sizeof c->mmu.atc);
    const uint64_t t = mc_machine_time(m);
    h = fnv(h, &t, 8);
    h = fnv(h, hw->ram, hw->ram_size);
    mc_framebuffer fb;
    if (mc_machine_framebuffer(m, &fb) && fb.pixels) h = fnv(h, fb.pixels, (size_t)fb.row_bytes * fb.height);
    return h;
}

/* --hot: an open-addressing table of sampled PCs. */
#define HOT_SLOTS 65536u
static uint32_t hot_pc[HOT_SLOTS], hot_n[HOT_SLOTS];
static uint64_t hot_total;
static void hot_add(uint32_t pc) {
    uint32_t i = (pc * 2654435761u) >> 16;
    for (unsigned k = 0; k < HOT_SLOTS; k++, i = (i + 1) & (HOT_SLOTS - 1)) {
        if (hot_n[i] && hot_pc[i] != pc) continue;
        hot_pc[i] = pc;
        hot_n[i]++;
        hot_total++;
        return;
    }
}
static void hot_print(unsigned top) {
    printf("hot PCs (%llu samples):%c", (unsigned long long)hot_total, 10);
    for (unsigned t = 0; t < top; t++) {
        unsigned best = 0;
        for (unsigned i = 1; i < HOT_SLOTS; i++)
            if (hot_n[i] > hot_n[best]) best = i;
        if (!hot_n[best]) break;
        printf("  %08X %6.2f%%%c", (unsigned)hot_pc[best], 100.0 * hot_n[best] / (double)hot_total, 10);
        hot_n[best] = 0;
    }
}

static int peek_logical(mc_hw *hw, uint32_t la, unsigned size, uint32_t *v);
static unsigned exc_want, exc_count;
static uint32_t trap_word = 0x10000;
static int log_alerts;
static void on_exception(void *ctx, m68k_cpu *cpu, unsigned vector, uint32_t pc) {
    (void)ctx;
    uint32_t w = 0;
    /* --alerts: every Dialog Manager alert ($A985 _Alert, $A986 _StopAlert,
     * $A987 _NoteAlert, $A988 _CautionAlert; Inside Macintosh: Macintosh
     * Toolbox Essentials, "Dialog Manager"), with its ALRT resource ID (the
     * first Pascal parameter, under the filter procedure's pointer), and
     * every system error box ($A9C9 _SysError, its ID in D0; Inside
     * Macintosh: Operating System Utilities, "System Error Handler") but
     * the greeting (dsGreeting, 40) and the shutdown screens (20000 up). */
    if (vector == 10 && log_alerts && mc_bus_peek(&the_hw->bus, pc, 2, &w)) {
        const uint32_t t = w & 0xFBFFu;      /* a Toolbox trap: bit 10 auto-pop, bit 9 its number */
        const int16_t err = (int16_t)cpu->r[0];
        uint32_t id = 0;
        if (t >= 0xA985u && t <= 0xA988u) {
            peek_logical(the_hw, cpu->r[15] + 4, 2, &id);
            printf("alert: trap %04X, ALRT %d, at PC %08X (instruction %llu)%c", (unsigned)w, (int)(int16_t)id, (unsigned)pc,
                   (unsigned long long)cpu->instructions, 10);
        } else if (t == 0xA9C9u && err != 40 && err < 20000) {
            printf("alert: trap %04X, SysError %d, at PC %08X (instruction %llu)%c", (unsigned)w, (int)err, (unsigned)pc,
                   (unsigned long long)cpu->instructions, 10);
        }
    }
    /* The trap's flag bits 9-10 (immediate, async; Inside Macintosh II,
     * "The Trap Dispatcher") do not count; a Control or Status call also
     * shows its csCode and refNum. */
    if (vector == 10 && trap_word < 0x10000 && mc_bus_peek(&the_hw->bus, pc, 2, &w) && ((w & 0xF9FFu) == (trap_word & 0xF9FFu) || (trap_word == 0xFFFFu && (w & 0xF000u) == 0xA000u))) {
        uint32_t cs = 0, ref = 0;
        const int device = (w & 0xF8FEu) == 0xA004u;
        if (device) {
            peek_logical(the_hw, cpu->r[8] + 26, 2, &cs);
            peek_logical(the_hw, cpu->r[8] + 24, 2, &ref);
        }
        printf("  trap %04X at PC %08X: D0 %08X D1 %08X A0 %08X (instruction %llu)", (unsigned)w, (unsigned)pc,
               (unsigned)cpu->r[0], (unsigned)cpu->r[1], (unsigned)cpu->r[8], (unsigned long long)cpu->instructions);
        if (device) printf(" refNum %d csCode %u", (int)(int16_t)ref, (unsigned)cs);
        if ((w & 0xF8FFu) == 0xA000u) {   /* _Open: the name at ioNamePtr */
            uint32_t name = 0, len = 0, ch = 0;
            peek_logical(the_hw, cpu->r[8] + 18, 4, &name);
            if (name && peek_logical(the_hw, name, 1, &len)) {
                printf(" \"");
                for (uint32_t k = 1; k <= len && k < 64 && peek_logical(the_hw, name + k, 1, &ch); k++) printf("%c", (int)ch);
                printf("\"");
            }
        }
        printf("%c", 10);
    }
    if (exc_count++ < exc_want)
        printf("  exception %3u at PC %08X (instruction %llu, SR %04X)\n", vector, (unsigned)pc,
               (unsigned long long)cpu->instructions, m68k_sr(cpu));
}

static uint8_t *load(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc((size_t)n);
    if (d && fread(d, 1, (size_t)n, f) != (size_t)n) {
        free(d);
        d = NULL;
    }
    fclose(f);
    *size = (size_t)n;
    return d;
}

/* The sound output, pulled after every run slice. */
static FILE *wav;
static uint32_t wav_frames, wav_rate;
static int wav_peak;
static double wav_first = -1;
static void wav_header(void) {
    uint8_t h[44] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 2, 0};
    const uint32_t bytes = wav_frames * 4u, riff = 36u + bytes, rate = wav_rate, avg = wav_rate * 4u;
    h[32] = 4; h[34] = 16;
    h[36] = 'd'; h[37] = 'a'; h[38] = 't'; h[39] = 'a';
    for (int i = 0; i < 4; i++) {
        h[4 + i] = (uint8_t)(riff >> (8 * i)); h[24 + i] = (uint8_t)(rate >> (8 * i));
        h[28 + i] = (uint8_t)(avg >> (8 * i));
    }
    h[40] = (uint8_t)bytes; h[41] = (uint8_t)(bytes >> 8); h[42] = (uint8_t)(bytes >> 16); h[43] = (uint8_t)(bytes >> 24);
    fseek(wav, 0, SEEK_SET);
    fwrite(h, 1, sizeof h, wav);
    fseek(wav, 0, SEEK_END);
}
static void wav_pump(mc_machine *m) {
    int16_t buf[4096 * 2];
    size_t n;
    if (!wav_rate) wav_rate = mc_machine_audio_rate(m);
    while ((n = mc_machine_audio_pull(m, buf, 4096)) > 0) {
        for (size_t i = 0; i < 2 * n; i++) {
            const int v = buf[i] < 0 ? -buf[i] : buf[i];
            if (v > wav_peak) wav_peak = v;
            if (v > 256 && wav_first < 0) wav_first = (double)(wav_frames + i / 2) / (wav_rate ? wav_rate : 1);
        }
        fwrite(buf, 4, n, wav);
        wav_frames += (uint32_t)n;
        if (n < 4096) break;
    }
}

static int peek_logical(mc_hw *hw, uint32_t la, unsigned size, uint32_t *v);
/* A guest byte written at a logical address (supervisor data). */
static int poke_logical(mc_hw *hw, uint32_t la, uint8_t v) {
    uint32_t pa = la;
    unsigned fill = 0;
    if (hw->cpu->mmu.mode == 1 && m68k_mmu_translate(hw->cpu, la, 5, 1, 0, &pa, &fill)) return 0;
    uint8_t *page = hw->bus.write_map[pa >> MC_BUS_PAGE_SHIFT];
    if (!page) return 0;
    page[pa & (MC_BUS_PAGE_SIZE - 1)] = v;
    return 1;
}
static void run_code(mc_machine *m, mc_hw *hw, const char *path) {
    size_t size = 0;
    uint8_t *code = load(path, &size);
    if (!code || size < 516) { printf("run-code: cannot read %s%c", path, 10); free(code); return; }
    size_t stop = 0;
    for (size_t k = 0; k + 1 < size; k += 2)
        if (code[k] == 0x60 && code[k + 1] == 0xFE) { stop = k; break; }
    m68k_cpu *cpu = hw->cpu;
    m68k_set_sr(cpu, 0x2700);
    cpu->status = M68K_OK;
    const uint32_t at = ((cpu->r[15] - 0x10000u) & ~15u);
    for (size_t k = 0; k < size; k++)
        if (!poke_logical(hw, at + (uint32_t)k, code[k])) { printf("run-code: no memory at %08X%c", (unsigned)(at + k), 10); free(code); return; }
    m68k_pflusha(cpu);
    m68k_jit_flush(cpu);    /* the program was written behind the CPU's back */
    cpu->pc = at;
    int ok = 0;
    const int show = getenv("LIVADIX_RUNCODE_PCS") != NULL;
    for (int i = 0; i < 20000000; i++) {
        if (show && i < 4000) printf("pc %08X d0 %08X d1 %08X a0 %08X%c", (unsigned)cpu->pc, (unsigned)cpu->r[0], (unsigned)cpu->r[1], (unsigned)cpu->r[8], 10);
        if (show && cpu->pc == 0x4088DB2Au) {
            printf("frame:");
            for (uint32_t k = 0; k < 0x34; k++) { uint32_t v = 0; peek_logical(hw, cpu->r[15] + k, 1, &v); printf(" %02X", (unsigned)v); }
            printf("%c", 10);
        }
        if (mc_machine_run(m, 1, 0) != MC_RUN_OK) break;
        if (cpu->pc == at + stop) { ok = 1; break; }
    }
    printf("run-code: %s%c", ok ? "done" : "did not finish", 10);
    for (size_t k = size - 512; k < size; k += 16) {
        printf("res %03X:", (unsigned)(k - (size - 512)));
        for (size_t b = 0; b < 16; b++) {
            uint32_t v = 0;
            peek_logical(hw, at + (uint32_t)(k + b), 1, &v);
            printf(" %02X", (unsigned)v);
        }
        printf("%c", 10);
    }
    free(code);
}

/* Calls the guest's _Gestalt through the ROM's call site at ROM offset
 * $C78A (A1AD, then TST.W D0 at $C78C), the ROM where low memory ROMBase
 * ($2AE) has it ($4080 0000 on most machines, $40A0 0000 on the LC
 * family); a ROM without that call site (the 256 KB ROMs, older than
 * Gestalt: the System installs the trap) runs the trap from 512 bytes
 * below the stack, where it is written for the call and the bytes put back
 * after: 1 with *err and *value set. */
static int guest_gestalt(mc_machine *m, mc_hw *hw, uint32_t selector, int32_t *err, uint32_t *value) {
    m68k_cpu *cpu = hw->cpu;
    uint32_t r[16];
    memcpy(r, cpu->r, sizeof r);
    const uint32_t pc = cpu->pc;
    const uint16_t sr = m68k_sr(cpu);
    const m68k_status status = cpu->status;
    m68k_set_sr(cpu, 0x2700);
    cpu->status = M68K_OK;
    uint32_t rom_base = 0;
    if (!peek_logical(hw, 0x2AE, 4, &rom_base) || (rom_base & 0xFFFFu)) rom_base = 0x40800000u;
    cpu->r[0] = selector;
    uint32_t site = rom_base + 0xC78Au, word = 0, saved = 0;
    const int scratch = !peek_logical(hw, site, 2, &word) || word != 0xA1ADu;
    if (scratch) {
        site = (cpu->r[15] - 0x200u) & ~1u;
        peek_logical(hw, site, 4, &saved);
        static const uint8_t call[4] = {0xA1, 0xAD, 0x4E, 0x71};
        for (unsigned b = 0; b < 4; b++) poke_logical(hw, site + b, call[b]);
        m68k_jit_invalidate(cpu, site, 4);
    }
    cpu->pc = site;
    int ok = 0;
    for (int i = 0; i < 2000000; i++) {
        if (mc_machine_run(m, 1, 0) != MC_RUN_OK) break;
        if (cpu->pc == site + 2u) { ok = 1; break; }
    }
    if (scratch) {
        for (unsigned b = 0; b < 4; b++) poke_logical(hw, site + b, (uint8_t)(saved >> (24 - 8 * b)));
        m68k_jit_invalidate(cpu, site, 4);
    }
    *err = (int16_t)cpu->r[0];
    *value = cpu->r[8];
    m68k_set_sr(cpu, sr);
    memcpy(cpu->r, r, sizeof r);
    cpu->pc = pc;
    cpu->status = status;
    return ok;
}

static void save_bmp(const char *path, const uint32_t *argb, uint32_t w, uint32_t h) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    const uint32_t stride = (w * 3 + 3) & ~3u, size = 54 + stride * h;
    uint8_t hdr[54] = {'B', 'M'};
    hdr[2] = (uint8_t)size; hdr[3] = (uint8_t)(size >> 8); hdr[4] = (uint8_t)(size >> 16); hdr[5] = (uint8_t)(size >> 24);
    hdr[10] = 54; hdr[14] = 40;
    for (int i = 0; i < 4; i++) { hdr[18 + i] = (uint8_t)(w >> (8 * i)); hdr[22 + i] = (uint8_t)(h >> (8 * i)); }
    hdr[26] = 1; hdr[28] = 24;
    fwrite(hdr, 1, 54, f);
    uint8_t *row = calloc(1, stride);
    for (uint32_t y = h; y-- > 0;) {
        for (uint32_t x = 0; x < w; x++) {
            const uint32_t p = argb[(size_t)y * w + x];
            row[x * 3] = (uint8_t)p; row[x * 3 + 1] = (uint8_t)(p >> 8); row[x * 3 + 2] = (uint8_t)(p >> 16);
        }
        fwrite(row, 1, stride, f);
    }
    free(row);
    fclose(f);
}

typedef struct input {
    uint64_t at;
    int kind, a, b;     /* 0 key, 1 motion, 2 point, 3 button, 4 screenshot */
    const char *file;
    int rel;            /* at counts from the Finder's start (@+N) */
} input;

/* Inputs in time order (stable); those waiting for the Finder last. */
static void sort_inputs(input *in, unsigned from, unsigned n) {
    for (unsigned a = from + 1; a < n; a++)
        for (unsigned b = a; b > from && (in[b - 1].rel > in[b].rel ||
                                          (in[b - 1].rel == in[b].rel && in[b - 1].at > in[b].at)); b--) {
            input t = in[b]; in[b] = in[b - 1]; in[b - 1] = t;
        }
}

static void save_bmp(const char *path, const uint32_t *argb, uint32_t w, uint32_t h);

/* A guest longword or byte at a logical address (supervisor data). */
/* --audio-input: the file's frames in turn, from its start again at its end. */
static void input_pcm(void *ctx, unsigned rate, int16_t frame[2]) {
    FILE *f = ctx;
    uint8_t raw[4];
    (void)rate;
    if (fread(raw, 1, sizeof raw, f) != sizeof raw) {
        rewind(f);
        if (fread(raw, 1, sizeof raw, f) != sizeof raw) return;
    }
    frame[0] = (int16_t)((uint16_t)raw[0] | (uint16_t)raw[1] << 8);
    frame[1] = (int16_t)((uint16_t)raw[2] | (uint16_t)raw[3] << 8);
}

/* --find: each copy of `word` in RAM that is not a move.l #word,(An)'s
 * immediate, and the `len` bytes it starts. */
static void find_in_ram(const mc_hw *hw, uint32_t word, uint32_t len) {
    for (uint32_t a = 2; a + 4 <= hw->ram_size; a += 2) {
        const uint8_t *p = hw->ram + a;
        if (((uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]) != word) continue;
        const unsigned before = (unsigned)p[-2] << 8 | p[-1];
        if ((before & 0xF1FFu) == 0x20BCu) continue;
        printf("found %08X after %04X:", (unsigned)a, before);
        for (uint32_t k = 0; k < len && a + k < hw->ram_size; k++) printf(" %02X", p[k]);
        printf("%c", 10);
    }
}

static int peek_logical(mc_hw *hw, uint32_t la, unsigned size, uint32_t *v) {
    uint32_t pa = la;
    unsigned fill = 0;
    if (hw->cpu->mmu.mode == 1 && m68k_mmu_translate(hw->cpu, la, 5, 0, 0, &pa, &fill)) return 0;
    return mc_bus_peek(&hw->bus, pa, size, v);
}
/* --drivers: the unit table's drivers (Inside Macintosh II, "The Device
 * Manager": UTableBase $11C, UnitNtryCnt $1D2; a DCE's dCtlDriver at 0, a
 * handle when dCtlFlags ($4) bit 6 (dRAMBased) is set; drvrName at 18). */
static void list_drivers(mc_hw *hw) {
    uint32_t table = 0, count = 0;
    peek_logical(hw, 0x11C, 4, &table);
    peek_logical(hw, 0x1D2, 2, &count);
    for (uint32_t u = 0; u < count && u < 512; u++) {
        uint32_t h = 0, dce = 0, drv = 0, flags = 0, len = 0, ch = 0;
        if (!peek_logical(hw, table + 4 * u, 4, &h) || !h || !peek_logical(hw, h, 4, &dce) || !dce) continue;
        peek_logical(hw, dce + 4, 2, &flags);
        peek_logical(hw, dce, 4, &drv);
        if ((flags & 0x40) && drv) peek_logical(hw, drv, 4, &drv);
        if (!drv) continue;
        peek_logical(hw, drv + 18, 1, &len);
        printf("driver unit %u (refNum %d), flags %04X:", (unsigned)u, -(int)u - 1, (unsigned)flags);
        for (uint32_t k = 1; k <= len && k < 32 && peek_logical(hw, drv + 18 + k, 1, &ch); k++) printf("%c", (int)ch);
        printf("%c", 10);
    }
}

static void list_volumes(mc_hw *hw) {
    /* The drive queue (DrvQHdr at $308; qLink, qType, dQDrive, dQRefNum). */
    uint32_t dq = 0;
    peek_logical(hw, 0x30A, 4, &dq);
    for (int n = 0; dq && n < 32; n++) {
        uint32_t drive = 0, ref = 0;
        peek_logical(hw, dq + 6, 2, &drive);
        peek_logical(hw, dq + 8, 2, &ref);
        printf("drive %u: driver %d%c", (unsigned)drive, (int)(int16_t)ref, 10);
        if (!peek_logical(hw, dq, 4, &dq)) break;
    }
    uint32_t vcb = 0;
    if (!peek_logical(hw, 0x358, 4, &vcb)) return;
    for (int n = 0; vcb && n < 32; n++) {
        uint32_t len = 0, c;
        char name[32];
        peek_logical(hw, vcb + 44, 1, &len);
        if (len > 27) len = 27;
        for (uint32_t i = 0; i < len; i++) { peek_logical(hw, vcb + 45 + i, 1, &c); name[i] = (char)c; }
        name[len] = 0;
        printf("volume: %s%c", name, 10);
        if (!peek_logical(hw, vcb, 4, &vcb)) break;
    }
}
static void shot_now(mc_machine *m, const char *path) {
    mc_framebuffer fb;
    if (!mc_machine_framebuffer(m, &fb)) return;
    uint32_t *argb = malloc((size_t)fb.width * fb.height * 4);
    mc_framebuffer_argb(&fb, argb);
    save_bmp(path, argb, fb.width, fb.height);
    free(argb);
}

/* The cursor where the System keeps it (low memory Mouse: v, h; a logical
 * address: the IIci's logical RAM does not start at physical 0). */
static void cursor(mc_hw *hw, int *x, int *y) {
    uint32_t v = 0;
    peek_logical(hw, 0x830, 4, &v);
    *y = (int16_t)(v >> 16);
    *x = (int16_t)v;
}
/* Moves the cursor to (x, y) in small steps, each given time for an ADB
 * poll and the cursor task. */
static mc_run_status point(mc_machine *m, mc_hw *hw, int x, int y, int button) {
    mc_run_status st = MC_RUN_OK;
    for (int i = 0; i < 2000 && st == MC_RUN_OK; i++) {
        int cx, cy;
        cursor(hw, &cx, &cy);
        int dx = x - cx, dy = y - cy;
        if (!dx && !dy) break;
        /* The System accelerates: big steps far away, single pixels near. */
        const int lim = (dx > 24 || dx < -24 || dy > 24 || dy < -24) ? 4 : 1;
        if (dx > lim) dx = lim; else if (dx < -lim) dx = -lim;
        if (dy > lim) dy = lim; else if (dy < -lim) dy = -lim;
        mc_machine_mouse(m, dx, dy, button);
        st = mc_machine_run(m, 600000, 0);
    }
    return st;
}

static double host_seconds(void) { return (double)clock() / CLOCKS_PER_SEC; }
static double wall_seconds(void) { return (double)mc_clock_ms() / 1000.0; }

/* --live-code: a program run in the Finder's context (see the options). */
typedef struct live {
    int state;                  /* 0 none, 1 waiting to start, 2 running, 3 done */
    const char *path;
    uint8_t *code;
    size_t size, stop;
    uint32_t args[8];
    unsigned nargs;
    uint32_t stack;             /* bytes of stack above the program (--live-stack) */
    uint32_t vector63;          /* $FC as it was: a moved program's address meanwhile */
    uint32_t base, saved_r[16], saved_pc;
    uint16_t saved_sr;
    m68k_status saved_status;
    uint64_t started;
} live;

static int peek_logical(mc_hw *hw, uint32_t la, unsigned size, uint32_t *v);
static int finder_is_current(mc_hw *hw) {
    static const uint8_t finder[7] = {6, 'F', 'i', 'n', 'd', 'e', 'r'};
    for (unsigned k = 0; k < 7; k++) {
        uint32_t ch = 0;
        if (!peek_logical(hw, 0x910 + k, 1, &ch) || ch != finder[k]) return 0;
    }
    return 1;
}
static int live_start(live *l, mc_hw *hw) {
    m68k_cpu *cpu = hw->cpu;
    if ((m68k_sr(cpu) & 0x3700) != 0x2000 || cpu->status != M68K_OK || !finder_is_current(hw)) return 0;
    /* Only at a call of the Finder's event loop (_WaitNextEvent $A860,
     * _GetNextEvent $A970, _EventAvail $A971, with or without auto-pop),
     * outside any Toolbox or OS routine the program's calls could re-enter. */
    uint32_t op = 0;
    if (!peek_logical(hw, cpu->pc, 2, &op)) return 0;
    op &= 0xFBFFu;
    if (op != 0xA860u && op != 0xA970u && op != 0xA971u) return 0;
    uint32_t limit = 0;
    peek_logical(hw, 0x130, 4, &limit);   /* ApplLimit */
    const uint32_t area = (uint32_t)((l->size + (l->stack ? l->stack : 0x2000u) + 0xFFFu) & ~0xFFFu);
    const uint32_t base = ((cpu->r[15] - 0x400u) & ~0xFFFu) - area;
    if (!limit || base < limit) return 0;
    for (size_t k = 0; k < area; k++)
        if (!poke_logical(hw, base + (uint32_t)k, k < l->size ? l->code[k] : 0)) return 0;
    for (unsigned k = 0; k < l->nargs; k++)
        for (unsigned b = 0; b < 4; b++) poke_logical(hw, base + 4 + 4 * k + b, (uint8_t)(l->args[k] >> (24 - 8 * b)));
    peek_logical(hw, 0xFC, 4, &l->vector63);
    for (unsigned b = 0; b < 4; b++) poke_logical(hw, 0xFC + b, 0);
    memcpy(l->saved_r, cpu->r, sizeof l->saved_r);
    l->saved_pc = cpu->pc;
    l->saved_sr = m68k_sr(cpu);
    l->saved_status = cpu->status;
    l->base = base;
    m68k_pflusha(cpu);
    m68k_jit_flush(cpu);
    cpu->r[15] = base + area;
    cpu->pc = base;
    return 1;
}
/* Where the live program runs: where it was put, or its copy. */
static uint32_t live_where(live *l, mc_hw *hw) {
    uint32_t moved = 0;
    peek_logical(hw, 0xFC, 4, &moved);
    return moved ? moved : l->base;
}
static void live_finish(live *l, mc_hw *hw, int ok, uint64_t instructions) {
    m68k_cpu *cpu = hw->cpu;
    const uint32_t where = live_where(l, hw);
    printf("live-code: %s after %llu instructions%c", ok ? "done" : "did not finish", (unsigned long long)instructions, 10);
    for (size_t k = l->size - 512; k < l->size; k += 16) {
        printf("live res %03X:", (unsigned)(k - (l->size - 512)));
        for (size_t b = 0; b < 16; b++) {
            uint32_t v = 0;
            peek_logical(hw, where + (uint32_t)(k + b), 1, &v);
            printf(" %02X", (unsigned)v);
        }
        printf("%c", 10);
    }
    for (unsigned b = 0; b < 4; b++) poke_logical(hw, 0xFC + b, (uint8_t)(l->vector63 >> (24 - 8 * b)));
    m68k_set_sr(cpu, l->saved_sr);
    memcpy(cpu->r, l->saved_r, sizeof l->saved_r);
    cpu->pc = l->saved_pc;
    cpu->status = l->saved_status;
    l->state = 3;
}

int main(int argc, char **argv) {
    const char *rom_path = NULL, *model = "quadra-700", *nvram = NULL, *shot = NULL, *wav_path = NULL;
    struct { uint32_t at, len; int indirect, logical; } dumps[16];
    long long clock = -1;
    unsigned ndumps = 0;
    const char *audio_input = NULL;
    uint32_t find_word = 0, find_len = 0;
    int find_set = 0;
    int threshold = -1;
    unsigned ram = 8 * 1024u, pcs = 32, w = 0, h = 0, depth = 0, hot = 0;
    int interp = 0, callout = 1, idle_skip = 1, jit_stats = 0, count_io = 0;
    uint64_t checkpoint = 0, hz = 0;
    uint64_t max = 10000000;
    int step = 0;
    uint64_t step_from = 0;
    uint32_t watch = 1, watched = 0;
    uint32_t stop_pc = 1, log_pc = 1;
    input inputs[64];
    unsigned ninputs = 0;
    struct { const char *path; int kind, writable, ata; } media[12];
    uint32_t gestalts[16];
    unsigned ngestalts = 0;
    unsigned nmedia = 0;
    int fpu_full = 0, pmmu = 0, charger_start = 0, boot_cdrom = 0, cdrom_ata = 0, no_chime = 0, volumes = 0;
    uint32_t peek_at = 0, peek_n = 0;
    int video_card = MC_VIDEO_CARD_AUTO, accel = 1;
    int controller = MC_CONTROLLER_PROTOCOL;
    long long finder_settle = -1;
    const char *code_path = NULL;
    uint64_t finder_at = 0;
    int use_net = 0, drivers = 0;
    mc_config netconf;
    mc_config_defaults(&netconf);
    long long live_after = -1;
    uint64_t live_stop = 0;
    live lv;
    memset(&lv, 0, sizeof lv);
    io_mask = 0;
    io_addr = 0;
    for (int i = 1; i < argc; i++) {
        const unsigned had_inputs = ninputs;
        if (!strcmp(argv[i], "--model") && i + 1 < argc) model = argv[++i];
        else if (!strcmp(argv[i], "--ram") && i + 1 < argc) ram = mc_config_parse_ram(argv[++i]);
        else if (!strcmp(argv[i], "--mhz") && i + 1 < argc) {
            if (!(hz = mc_config_parse_mhz(argv[++i]))) { fprintf(stderr, "--mhz: 1 to 10000%c", 10); return 2; }
        }
        else if (!strcmp(argv[i], "--max") && i + 1 < argc) max = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--io") && i + 1 < argc) io_want = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--io-from") && i + 1 < argc) io_from = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--io-filter") && i + 2 < argc) {
            io_addr = (uint32_t)strtoul(argv[++i], NULL, 0);
            io_mask = (uint32_t)strtoul(argv[++i], NULL, 0);
        } else if (!strcmp(argv[i], "--pcs") && i + 1 < argc) { pcs = (unsigned)atoi(argv[++i]); if (pcs) step = 1; }
        else if (!strcmp(argv[i], "--step")) step = 1;
        else if (!strcmp(argv[i], "--hot") && i + 1 < argc) hot = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--interp")) interp = 1;
        else if (!strcmp(argv[i], "--no-callout")) callout = 0;
        else if (!strcmp(argv[i], "--no-idle-skip")) idle_skip = 0;
        else if (!strcmp(argv[i], "--jit-stats")) jit_stats = 1;
        else if (!strcmp(argv[i], "--threshold") && i + 1 < argc) threshold = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--io-count")) count_io = 1;
        else if (!strcmp(argv[i], "--checkpoint") && i + 1 < argc) checkpoint = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--step-from") && i + 1 < argc) step_from = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--watch") && i + 1 < argc) { watch = (uint32_t)strtoul(argv[++i], NULL, 0); step = 1; }
        else if (!strcmp(argv[i], "--stop-pc") && i + 1 < argc) { stop_pc = (uint32_t)strtoul(argv[++i], NULL, 0); step = 1; }
        else if (!strcmp(argv[i], "--log-pc") && i + 1 < argc) { log_pc = (uint32_t)strtoul(argv[++i], NULL, 0); step = 1; }
        else if (!strcmp(argv[i], "--exceptions") && i + 1 < argc) exc_want = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--nvram") && i + 1 < argc) nvram = argv[++i];
        else if (!strcmp(argv[i], "--shot") && i + 1 < argc) shot = argv[++i];
        else if (!strcmp(argv[i], "--wav") && i + 1 < argc) wav_path = argv[++i];
        else if (!strcmp(argv[i], "--fpu-full")) fpu_full = 1;
        else if (!strcmp(argv[i], "--pmmu")) pmmu = 1;
        else if (!strcmp(argv[i], "--charger-start")) charger_start = 1;
        else if (!strcmp(argv[i], "--boot-cdrom")) boot_cdrom = 1;
        else if (!strcmp(argv[i], "--no-chime")) no_chime = 1;
        else if (!strcmp(argv[i], "--volumes")) volumes = 1;
        else if (!strcmp(argv[i], "--peek") && i + 2 < argc) {
            peek_at = (uint32_t)strtoul(argv[i + 1], NULL, 16);
            peek_n = (uint32_t)strtoul(argv[i + 2], NULL, 0);
            i += 2;
        }
        else if (!strcmp(argv[i], "--run-code") && i + 1 < argc) code_path = argv[++i];
        else if (!strcmp(argv[i], "--net")) use_net = 1;
        else if (!strcmp(argv[i], "--share") && i + 1 < argc && netconf.share_count < MC_CONFIG_SHARES) {
            const char *a = argv[++i], *eq = strchr(a, '=');
            mc_share_config *f = &netconf.shares[netconf.share_count];
            if (!eq || eq == a || (size_t)(eq - a) >= sizeof f->name) { fprintf(stderr, "--share NAME=FOLDER\n"); return 2; }
            memcpy(f->name, a, (size_t)(eq - a));
            snprintf(f->path, sizeof f->path, "%s", eq + 1);
            const size_t pl = strlen(f->path);
            if (pl > 3 && !strcmp(f->path + pl - 3, ":ro")) { f->path[pl - 3] = 0; f->readonly = 1; }
            netconf.share_count++;
        } else if (!strcmp(argv[i], "--share-appledouble")) netconf.sharing_metadata = MC_SHARING_APPLEDOUBLE;
        else if (!strcmp(argv[i], "--share-password") && i + 1 < argc) snprintf(netconf.sharing_password, sizeof netconf.sharing_password, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--drivers")) drivers = 1;
        else if (!strcmp(argv[i], "--live-then") && i + 1 < argc) live_after = strtoll(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--live-stack") && i + 1 < argc) lv.stack = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--live-arg") && i + 1 < argc && lv.nargs < 8) lv.args[lv.nargs++] = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--live-code") && i + 1 < argc && ninputs < 64) {
            char *a = argv[++i], *at = strrchr(a, '@');
            if (at) { *at = 0; inputs[ninputs++] = (input){strtoull(at + 1, NULL, 0), 7, 0, 0, a}; }
        }
        else if (!strcmp(argv[i], "--finder") && i + 1 < argc) finder_settle = strtoll(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--alerts")) log_alerts = 1;
        else if (!strcmp(argv[i], "--trap") && i + 1 < argc) trap_word = (uint32_t)strtoul(argv[++i], NULL, 16);
        else if (!strcmp(argv[i], "--gestalt") && i + 1 < argc && ngestalts < 16) {
            const char *g = argv[++i];
            uint32_t sel = 0;
            for (int k = 0; k < 4; k++) sel = sel << 8 | (uint8_t)(g[k] && k < (int)strlen(g) ? g[k] : ' ');
            gestalts[ngestalts++] = sel;
        }
        else if (!strcmp(argv[i], "--cdrom-ata")) cdrom_ata = 1;
        else if ((!strcmp(argv[i], "--disk") || !strcmp(argv[i], "--disk-ro") || !strcmp(argv[i], "--cdrom") ||
                  !strcmp(argv[i], "--floppy") || !strcmp(argv[i], "--disk-ata") || !strcmp(argv[i], "--disk-ata-ro")) &&
                 i + 1 < argc && nmedia < 12) {
            const char *o = argv[i];
            media[nmedia].kind = !strcmp(o, "--cdrom") ? MC_MEDIA_CDROM : !strcmp(o, "--floppy") ? MC_MEDIA_FLOPPY : MC_MEDIA_DISK;
            media[nmedia].writable = !strcmp(o, "--disk") || !strcmp(o, "--floppy") || !strcmp(o, "--disk-ata");
            media[nmedia].ata = !strncmp(o, "--disk-ata", 10);
            media[nmedia++].path = argv[++i];
        }

        else if (!strcmp(argv[i], "--clock") && i + 1 < argc) clock = strtoll(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--audio-input") && i + 1 < argc) audio_input = argv[++i];
        else if (!strcmp(argv[i], "--find") && i + 2 < argc) {
            find_word = (uint32_t)strtoul(argv[++i], NULL, 16);
            find_len = (uint32_t)strtoul(argv[++i], NULL, 0);
            find_set = 1;
        }
        else if (!strcmp(argv[i], "--dump") && i + 2 < argc && ndumps < 16) {
            const char *a = argv[++i];
            dumps[ndumps].indirect = *a == '@';
            dumps[ndumps].logical = *a == '~';
            dumps[ndumps].at = (uint32_t)strtoul(a + (*a == '@' || *a == '~'), NULL, 0);
            dumps[ndumps++].len = (uint32_t)strtoul(argv[++i], NULL, 0);
        }
        else if (!strcmp(argv[i], "--resolution") && i + 1 < argc) sscanf(argv[++i], "%ux%u", &w, &h);
        else if (!strcmp(argv[i], "--depth") && i + 1 < argc) depth = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--controller") && i + 1 < argc) {
            const char *k = argv[++i];
            controller = !strcmp(k, "firmware") ? MC_CONTROLLER_FIRMWARE : !strcmp(k, "auto") ? MC_CONTROLLER_AUTO : MC_CONTROLLER_PROTOCOL;
        } else if (!strcmp(argv[i], "--accel") && i + 1 < argc) {
            accel = strcmp(argv[++i], "off") != 0;
        } else if (!strcmp(argv[i], "--video-card") && i + 1 < argc) {
            const char *k = argv[++i];
            video_card = !strcmp(k, "livadix") ? MC_VIDEO_CARD_LIVADIX : !strcmp(k, "builtin") ? MC_VIDEO_CARD_BUILTIN : MC_VIDEO_CARD_AUTO;
        }
        else if (!strcmp(argv[i], "--key") && i + 1 < argc && ninputs < 64) {
            unsigned code; unsigned long long at;
            if (sscanf(argv[++i], "%x@%llu", &code, &at) == 2) inputs[ninputs++] = (input){at, 0, (int)code, 0, NULL};
        } else if (!strcmp(argv[i], "--point") && i + 1 < argc && ninputs < 64) {
            int x, y; unsigned long long at;
            if (sscanf(argv[++i], "%d,%d@%llu", &x, &y, &at) == 3) inputs[ninputs++] = (input){at, 2, x, y, NULL};
        } else if ((!strncmp(argv[i], "--down@", 7) || !strncmp(argv[i], "--up@", 5)) && ninputs < 64) {
            const int down = argv[i][2] == 'd';
            inputs[ninputs++] = (input){strtoull(argv[i] + (down ? 7 : 5), NULL, 0), 3, down, 0, NULL};
        } else if ((!strcmp(argv[i], "--insert-floppy") || !strcmp(argv[i], "--insert-cdrom")) && i + 1 < argc && ninputs < 64) {
            const int cd = argv[i][9] == 'c';
            char *a = argv[++i], *at = strrchr(a, '@');
            if (at) { *at = 0; inputs[ninputs++] = (input){strtoull(at + 1, NULL, 0), cd ? 6 : 5, 0, 0, a}; }
        } else if (!strcmp(argv[i], "--shot-at") && i + 1 < argc && ninputs < 64) {
            char *a = argv[++i], *at = strrchr(a, '@');
            if (at) { *at = 0; inputs[ninputs++] = (input){strtoull(at + 1, NULL, 0), 4, 0, 0, a}; }
        } else if (!strcmp(argv[i], "--control") && i + 1 < argc && ninputs < 64) {
            int c, v; unsigned long long at;
            if (sscanf(argv[++i], "%d,%d@%llu", &c, &v, &at) == 3) inputs[ninputs++] = (input){at, 8, c, v, NULL};
        } else if (!strcmp(argv[i], "--mouse") && i + 1 < argc && ninputs < 64) {
            int dx, dy; unsigned long long at;
            if (sscanf(argv[++i], "%d,%d@%llu", &dx, &dy, &at) == 3) inputs[ninputs++] = (input){at, 1, dx, dy, NULL};
        } else if (!rom_path) rom_path = argv[i];
        else {
            fprintf(stderr, "unknown argument %s\n", argv[i]);
            return 2;
        }
        if (ninputs > had_inputs) {
            const char *at = strrchr(argv[i], '@');
            if (!at && inputs[ninputs - 1].file) at = argv[i] + strlen(argv[i]);   /* FILE@N, the @ cut */
            inputs[ninputs - 1].rel = at && at[1] == '+';
        }
    }
    if (!rom_path) {
        fprintf(stderr, "usage: mc_hw_trace ROM [options]\n");
        return 2;
    }
    size_t rom_size = 0;
    uint8_t *rom = load(rom_path, &rom_size);
    if (!rom) {
        printf("%s: absent; skipped\n", rom_path);
        return 77;
    }
    mc_config c;
    mc_config_defaults(&c);
    const mc_model *p = mc_model_find(model);
    if (!p) {
        fprintf(stderr, "unknown model %s\n", model);
        return 2;
    }
    c.model = (uint32_t)mc_model_index(p);
    mc_config_set_ram_kb(&c, ram);
    c.cpu_hz = hz ? hz : p->cpu_hz;
    /* The model's own display unless given (not model: auto's). */
    c.width = w ? w : p->width;
    c.height = w ? h : p->height;
    c.depth = depth ? depth : p->depth;
    c.video_card = (uint32_t)video_card;
    c.video_acceleration = accel;
    /* The controller's firmware and the declaration ROMs (the SE/30's
     * video, the PowerBook 165c's and 180c's): the ROM file's folder
     * indexed as a roms folder. */
    static mc_rom_index rom_index;
    c.controller = (uint32_t)controller;
    {
        static char folder[1024];
        snprintf(folder, sizeof folder, "%s", rom_path);
        char *cut = strrchr(folder, '/'), *back = strrchr(folder, '\\');
        if (back > cut) cut = back;
        if (cut) cut[1] = 0;
        else snprintf(folder, sizeof folder, "./");
        const char *folders[1] = {folder};
        mc_rom_index_scan(&rom_index, folders, 1);
        c.roms = &rom_index;
    }
    if (nvram) {   /* the PRAM file itself, as the board writes it (not an owned record beside it) */
        snprintf(c.nvram, sizeof c.nvram, "%s", nvram);
        c.nvram_private = 1;
    }
    c.fpu_full = fpu_full;
    c.pmmu = pmmu;
    c.charger_start = charger_start;
    if (boot_cdrom) c.boot_priority = MC_BOOT_CDROM;
    if (cdrom_ata) c.cdrom_bus = MC_BUS_ATA;
    if (no_chime) c.startup_sound = 0;
    c.jit = !interp;
    for (unsigned k = 0; k < nmedia; k++)
        if (media[k].kind == MC_MEDIA_DISK && c.disk_count < MC_CONFIG_DISKS) {
            snprintf(c.disks[c.disk_count].image, sizeof c.disks[0].image, "%s", media[k].path);
            c.disks[c.disk_count].bus = media[k].ata ? MC_BUS_ATA : MC_BUS_SCSI;
            c.disks[c.disk_count++].readonly = !media[k].writable;
        }
    char error[512];
    mc_machine *m = mc_machine_create(&c, rom, rom_size, error, sizeof error);
    free(rom);
    if (!m) {
        fprintf(stderr, "%s\n", error);
        return 1;
    }
    mc_hw *hw = mc_machine_hw(m);
    if (!hw) {
        fprintf(stderr, "%s is not an emulated model\n", model);
        return 1;
    }
    the_hw = hw;
    FILE *audio_file = NULL;
    if (audio_input) {
        audio_file = fopen(audio_input, "rb");
        const mc_audio_source source = {audio_file, input_pcm, NULL};
        if (!audio_file || !mc_machine_set_audio_source(m, &source)) {
            fprintf(stderr, "%s: %s%c", audio_input, audio_file ? "the model has no sound input" : "cannot open", 10);
            return 1;
        }
    }
    if (!interp) m68k_jit_options(hw->cpu, callout, idle_skip, 0);
    if (!interp && threshold >= 0) m68k_jit_threshold(hw->cpu, (unsigned)threshold);
    unsigned disk_index = 0;
    for (unsigned k = 0; k < nmedia; k++) {
        mc_disk *d = mc_disk_open(media[k].path, media[k].writable, media[k].kind == MC_MEDIA_CDROM ? 2048 : 512, error, sizeof error);
        const unsigned drive = media[k].kind == MC_MEDIA_DISK ? disk_index++ : 0;
        if (!d || !mc_machine_insert(m, (mc_media_kind)media[k].kind, drive, d, error, sizeof error)) {
            fprintf(stderr, "%s: %s\n", media[k].path, error);
            return 1;
        }
    }
    if (wav_path) {
        wav = fopen(wav_path, "wb");
        if (!wav) { fprintf(stderr, "cannot write %s\n", wav_path); return 1; }
        wav_header();
    }
    if (clock >= 0) mc_machine_set_clock(m, clock);
    io_log = calloc(io_want ? io_want : 1, sizeof *io_log);
    mc_net *net = NULL;
    double live_wall = 0;
    uint64_t live_guest = 0;
    if (use_net) {
        net = mc_net_open_config(&netconf, error, sizeof error);
        if (!net) { fprintf(stderr, "network: %s\n", error); return 1; }
        mc_net_attach(net, m);
    }
    /* The observer costs every device access a call: only when something
     * reads what it records. */
    if (io_want || count_io) hw->bus.observe = observe;
    hw->cpu->on_exception = on_exception;
    uint32_t *ring = calloc(pcs ? pcs : 1, sizeof *ring);
    uint64_t n = 0;
    mc_run_status st = MC_RUN_OK;
    unsigned next_input = 0;
    uint64_t asleep_since = 0;
    int button = 0;
    uint64_t last_input = 0;
    sort_inputs(inputs, 0, ninputs);
    const double started = host_seconds();
    while (mc_machine_instructions(m) < max && st == MC_RUN_OK) {
        /* Inputs come at their time, and at least 2,000,000 instructions
         * (about 0.1 s) after the one before, so a click outlasts an ADB poll. */
        while (next_input < ninputs && !inputs[next_input].rel && inputs[next_input].at <= mc_machine_instructions(m) &&
               mc_machine_instructions(m) >= last_input + 2000000u) {
            const input *in = &inputs[next_input++];
            last_input = mc_machine_instructions(m);
            if (in->kind == 0) mc_machine_key(m, (uint8_t)(in->a & 0x7F), !(in->a & 0x80));
            else if (in->kind == 1) mc_machine_mouse(m, in->a, in->b, button);
            else if (in->kind == 2) { st = point(m, hw, in->a, in->b, button); last_input = mc_machine_instructions(m); }
            else if (in->kind == 3) { button = in->a; mc_machine_mouse(m, 0, 0, button); }
            else if (in->kind == 4) shot_now(m, in->file);
            else if (in->kind == 8) printf("control %d = %d: %d%c", in->a, in->b, mc_machine_control(m, (mc_control)in->a, in->b), 10);
            else if (in->kind == 7) {
                lv.code = load(in->file, &lv.size);
                lv.stop = 0;
                for (size_t k = 0; lv.code && k + 1 < lv.size; k += 2)
                    if (lv.code[k] == 0x60 && lv.code[k + 1] == 0xFE) { lv.stop = k; break; }
                if (!lv.code || lv.size < 1024 || !lv.stop) printf("live-code: cannot use %s%c", in->file, 10);
                else lv.state = 1;
            }
            else if (in->kind == 5 || in->kind == 6) {
                const int cd = in->kind == 6;
                char problem[256];
                mc_disk *d = mc_disk_open(in->file, !cd, cd ? 2048 : 512, problem, sizeof problem);
                if (!d || !mc_machine_insert(m, cd ? MC_MEDIA_CDROM : MC_MEDIA_FLOPPY, 0, d, problem, sizeof problem)) {
                    printf("insert %s: %s%c", in->file, problem, 10);
                    if (d) mc_disk_close(d);
                } else printf("inserted %s at instruction %llu%c", in->file, (unsigned long long)mc_machine_instructions(m), 10);
            }
        }
        /* A sleeping PowerBook has no instructions to step; keep advancing
         * hardware time so the next input can wake it. */
        if (step && !hw->reset_held && mc_machine_instructions(m) >= step_from) {
            if (pcs) ring[n++ % pcs] = hw->cpu->pc;
            if (hw->cpu->pc == stop_pc) break;
            if (hw->cpu->pc == log_pc) {
                uint32_t ret = 0;
                peek_logical(hw, hw->cpu->r[15], 4, &ret);
                printf("  at %08X: D0 %08X D1 %08X A0 %08X A1 %08X, return %08X (instruction %llu)%c", (unsigned)log_pc,
                       (unsigned)hw->cpu->r[0], (unsigned)hw->cpu->r[1], (unsigned)hw->cpu->r[8], (unsigned)hw->cpu->r[9],
                       (unsigned)ret, (unsigned long long)mc_machine_instructions(m), 10);
                printf("    stack:");
                for (uint32_t k = 0; k < 24; k++) {
                    uint32_t v = 0;
                    peek_logical(hw, hw->cpu->r[15] + 4 * k, 4, &v);
                    printf(" %08X", (unsigned)v);
                }
                printf("%c", 10);
                printf("    (A0):");
                for (uint32_t k = 0; k < 56; k++) {
                    uint32_t v = 0;
                    peek_logical(hw, hw->cpu->r[8] + k, 1, &v);
                    printf("%s%02X", k % 8 ? "" : " ", (unsigned)v);
                }
                printf("%c", 10);
            }
            const uint32_t pc = hw->cpu->pc;
            st = mc_machine_run(m, 1, 0);
            uint32_t now_value = 0;
            if (!(watch & 1) && mc_bus_peek(&hw->bus, watch, 4, &now_value) && now_value != watched) {
                printf("  watch %08X: %08X -> %08X at PC %08X (instruction %llu)%c", (unsigned)watch, (unsigned)watched,
                       (unsigned)now_value, (unsigned)pc, (unsigned long long)mc_machine_instructions(m), 10);
                watched = now_value;
            }
        } else {
            uint64_t chunk = max - mc_machine_instructions(m);
            if (step && step_from - mc_machine_instructions(m) < chunk) chunk = step_from - mc_machine_instructions(m);
            if (next_input < ninputs && !inputs[next_input].rel) {
                uint64_t due = inputs[next_input].at > last_input + 2000000u ? inputs[next_input].at : last_input + 2000000u;
                if (due > mc_machine_instructions(m) && due - mc_machine_instructions(m) < chunk) chunk = due - mc_machine_instructions(m);
            }
            if (chunk > 1000000) chunk = 1000000;
            if ((net || lv.state) && chunk > 100000) chunk = 100000;
            if (lv.state == 1) chunk = 1;   /* the start is at a trap call: watch every instruction */
            if (hot && chunk > 997) chunk = 997;
            if (checkpoint) {
                const uint64_t to = checkpoint - mc_machine_instructions(m) % checkpoint;
                if (to < chunk) chunk = to;
            }
            const uint64_t ran_from = mc_machine_instructions(m);
            st = mc_machine_run(m, chunk ? chunk : 1, 0);
            if (mc_machine_instructions(m) == ran_from && st == MC_RUN_OK) {
                /* The processor does not run (a PowerBook asleep): after 30 s
                 * of guest time the next input comes at once (a key wakes
                 * it), or the run ends. */
                if (!asleep_since) asleep_since = mc_machine_time(m) + 1;
                else if (mc_machine_time(m) - asleep_since > 30ull * MC_C15M_HZ) {
                    asleep_since = 0;
                    if (next_input < ninputs && !inputs[next_input].rel) {
                        printf("asleep: waking with the next input at instruction %llu%c", (unsigned long long)ran_from, 10);
                        inputs[next_input].at = ran_from;
                        last_input = ran_from >= 2000000u ? ran_from - 2000000u : 0;
                    } else {
                        printf("asleep: the processor stopped at instruction %llu%c", (unsigned long long)ran_from, 10);
                        break;
                    }
                }
            } else {
                asleep_since = 0;
            }
            if (hot) hot_add(hw->cpu->pc);
            if (checkpoint && mc_machine_instructions(m) % checkpoint == 0)
                printf("checkpoint %llu: %016llX%c", (unsigned long long)mc_machine_instructions(m),
                       (unsigned long long)state_hash(m, hw), 10);
        }
        if (finder_settle >= 0) {
            if (!finder_at) {
                static const uint8_t finder[7] = {6, 'F', 'i', 'n', 'd', 'e', 'r'};
                int match = 1;
                for (unsigned k = 0; k < 7 && match; k++) {
                    uint32_t ch = 0;
                    match = peek_logical(hw, 0x910 + k, 1, &ch) && ch == finder[k];
                }
                if (match) {
                    finder_at = mc_machine_instructions(m);
                    printf("finder: instruction %llu, guest time %.3f s, host time %.2f s%c", (unsigned long long)finder_at,
                           (double)mc_machine_time(m) / MC_C15M_HZ, host_seconds() - started, 10);
                    if (count_io) printf("device accesses to the Finder: %llu reads, %llu writes%c", (unsigned long long)io_reads, (unsigned long long)io_writes, 10);
                    for (unsigned k = next_input; k < ninputs; k++)
                        if (inputs[k].rel) { inputs[k].at += finder_at; inputs[k].rel = 0; }
                    sort_inputs(inputs, next_input, ninputs);
                }
            } else if (mc_machine_instructions(m) >= finder_at + (uint64_t)finder_settle) break;
        }
        if (wav) wav_pump(m);
        if (net) mc_net_pump(net, m);
        if (lv.state == 3 && live_stop && mc_machine_instructions(m) >= live_stop) break;
        if (lv.state == 1 && live_start(&lv, hw)) {
            lv.state = 2;
            lv.started = mc_machine_instructions(m);
            live_wall = wall_seconds();
            live_guest = mc_machine_time(m);
            printf("live-code: started at instruction %llu, at %08X%c", (unsigned long long)lv.started, (unsigned)lv.base, 10);
        } else if (lv.state == 2) {
            if (hw->cpu->pc == live_where(&lv, hw) + lv.stop) {
                live_finish(&lv, hw, 1, mc_machine_instructions(m) - lv.started);
                printf("live-code: guest time %.3f s, wall time %.3f s%c", (double)(mc_machine_time(m) - live_guest) / MC_C15M_HZ,
                       wall_seconds() - live_wall, 10);
                if (live_after >= 0) live_stop = mc_machine_instructions(m) + (uint64_t)live_after;
            }
            else if (wall_seconds() - live_wall > 180) live_finish(&lv, hw, 0, mc_machine_instructions(m) - lv.started);
            else {
                /* Guest time no faster than wall time. */
                const double ahead = (double)(mc_machine_time(m) - live_guest) / MC_C15M_HZ - (wall_seconds() - live_wall);
#ifdef _WIN32
                if (ahead > 0.002) Sleep(ahead > 0.02 ? 20 : 1);
#else
                if (ahead > 0.002) usleep(ahead > 0.02 ? 20000 : 1000);
#endif
            }
        }
    }
    printf("%llu instructions, guest time %.3f s; PC %08X SR %04X; %s\n", (unsigned long long)mc_machine_instructions(m),
           (double)mc_machine_time(m) / MC_C15M_HZ, (unsigned)hw->cpu->pc, m68k_sr(hw->cpu),
           st == MC_RUN_OK ? "ok" : st == MC_RUN_HALTED ? "powered off" : mc_machine_error(m));
    if (wav) {
        wav_header();
        fclose(wav);
        printf("sound: %u frames at %u Hz, peak %d, first sound at %.3f s\n", (unsigned)wav_frames, (unsigned)wav_rate, wav_peak, wav_first);
    }
    printf("D:");
    for (int i = 0; i < 8; i++) printf(" %08X", (unsigned)hw->cpu->r[i]);
    printf("\nA:");
    for (int i = 8; i < 16; i++) printf(" %08X", (unsigned)hw->cpu->r[i]);
    printf("\ncontrol: CACR %08X", (unsigned)hw->cpu->cacr);
    if (hw->cpu->model == M68K_68030)
        printf(" TC %08X TT0 %08X TT1 %08X", (unsigned)hw->cpu->mmu.tc30,
               (unsigned)hw->cpu->mmu.tt30[0], (unsigned)hw->cpu->mmu.tt30[1]);
    printf("\n");
    printf("\nexceptions: %u\n", exc_count);
    for (unsigned i = 0; i < io_count; i++) {
        const io_entry *e = &io_log[i];
        printf("  %10llu PC %08X %s.%c %08X = %0*X%s\n", (unsigned long long)e->when, (unsigned)e->pc,
               e->write ? "W" : "R", e->size == 1 ? 'B' : e->size == 2 ? 'W' : 'L', (unsigned)e->address,
               2 * e->size, (unsigned)e->value, e->fault ? " BUS ERROR" : "");
    }
    if (step && pcs) {
        printf("last PCs:");
        const uint64_t first = n > pcs ? n - pcs : 0;
        for (uint64_t i = first; i < n; i++) printf("%s%08X", (i - first) % 8 ? " " : "\n  ", (unsigned)ring[i % pcs]);
        printf("\n");
    }
    for (unsigned k = 0; k < ndumps; k++) {
        uint32_t at = dumps[k].at;
        if (dumps[k].indirect && !mc_bus_peek(&hw->bus, at, 4, &at)) continue;
        for (uint32_t a = 0; a < dumps[k].len; a += 16) {
            printf("%08X:", (unsigned)(at + a));
            for (uint32_t b = 0; b < 16 && a + b < dumps[k].len; b++) {
                uint32_t v = 0;
                uint32_t pa = at + a + b;
                unsigned fill = 0;
                if (dumps[k].logical && hw->cpu->mmu.mode == 1 && m68k_mmu_translate(hw->cpu, pa, 5, 0, 0, &pa, &fill)) { printf(" ??"); continue; }
                if (mc_bus_peek(&hw->bus, pa, 1, &v)) printf(" %02X", (unsigned)v);
                else printf(" --");
            }
            printf("\n");
        }
    }
    if (hot) hot_print(hot);
    if (jit_stats) printf("cpu: %s%c", hw->cpu->jit ? "translator" : "interpreter", 10);
    if (jit_stats && hw->cpu->jit) {
        m68k_jit_stats js;
        m68k_jit_get_stats(hw->cpu, &js);
        printf("jit: %llu blocks (%llu instructions), %llu block runs, %llu chained, %llu invalidated, %llu flushes, "
               "%llu instructions fast-forwarded%c", (unsigned long long)js.blocks, (unsigned long long)js.translated_instructions,
               (unsigned long long)js.exits, (unsigned long long)js.chains, (unsigned long long)js.invalidations,
               (unsigned long long)js.flushes, (unsigned long long)js.fast_forwarded, 10);
        printf("jit: %llu instructions in blocks (%.1f%%); exits normal %llu, interpret %llu, check %llu, fault %llu, chain %llu%c",
               (unsigned long long)js.run_instructions, 100.0 * (double)js.run_instructions / (double)hw->cpu->instructions,
               (unsigned long long)js.exit_kinds[0], (unsigned long long)js.exit_kinds[1], (unsigned long long)js.exit_kinds[2],
               (unsigned long long)js.exit_kinds[3], (unsigned long long)js.exit_kinds[4], 10);
    }
    if (volumes) list_volumes(hw);
    if (find_set) find_in_ram(hw, find_word, find_len);
    if (audio_file) {
        mc_machine_set_audio_source(m, NULL);
        fclose(audio_file);
    }
    for (uint32_t pk = 0; pk < peek_n; pk++) {
        uint32_t v = 0;
        peek_logical(hw, peek_at + 4 * pk, 4, &v);
        printf("%s%08X", pk % 8 ? " " : (pk ? "\npeek " : "peek "), (unsigned)v);
        if (pk + 1 == peek_n) printf("\n");
    }
    if (drivers) list_drivers(hw);
    if (lv.state == 1) printf("live-code: never started%c", 10);
    if (net) {
        mc_nat_stats s;
        mc_net_stats(net, &s);
        printf("network: %u DHCP offers, %u DHCP acks, %u BOOTP replies, %u ARP replies, %u DNS queries, %u TCP connections "
               "(%u refused, %u closed), %llu bytes out, %llu bytes in, %u retransmissions%c", s.dhcp_offers, s.dhcp_acks,
               s.bootp_replies, s.arp_replies, s.dns_queries, s.tcp_opened, s.tcp_refused, s.tcp_closed,
               (unsigned long long)s.tcp_bytes_out, (unsigned long long)s.tcp_bytes_in, s.tcp_retransmits, 10);
        mc_machine_set_net(m, NULL);
        mc_net_close(net);
    }
    if (code_path && st == MC_RUN_OK) run_code(m, hw, code_path);
    for (unsigned k = 0; k < ngestalts && st == MC_RUN_OK; k++) {
        int32_t err = 0;
        uint32_t value = 0;
        const uint32_t g = gestalts[k];
        if (guest_gestalt(m, hw, g, &err, &value))
            printf("gestalt '%c%c%c%c' = %u ($%08X), error %d%c", (char)(g >> 24), (char)(g >> 16), (char)(g >> 8), (char)g,
                   (unsigned)value, (unsigned)value, (int)err, 10);
        else
            printf("gestalt '%c%c%c%c': the call did not return%c", (char)(g >> 24), (char)(g >> 16), (char)(g >> 8), (char)g, 10);
    }
    mc_framebuffer fb;
    if (mc_machine_framebuffer(m, &fb)) {
        printf("screen: %ux%u, %u bits, %u bytes per row\n", (unsigned)fb.width, (unsigned)fb.height, (unsigned)fb.depth,
               (unsigned)fb.row_bytes);
        if (shot) {
            uint32_t *argb = malloc((size_t)fb.width * fb.height * 4);
            mc_framebuffer_argb(&fb, argb);
            save_bmp(shot, argb, fb.width, fb.height);
            free(argb);
        }
    } else printf("screen: off\n");
    mc_machine_destroy(m);
    free(ring);
    free(io_log);
    return st == MC_RUN_OK || st == MC_RUN_HALTED ? 0 : 1;
}
