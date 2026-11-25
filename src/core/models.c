/* Models: the defaults `model:` supplies (docs/NOTES.md).
 *
 * Period specifications, stated once here:
 * - cpu_hz is the processor's clock: the Guide to the Macintosh Family
 *   Hardware (2nd ed., appendix A) for the Macintosh II, IIx, IIcx and
 *   SE/30 (15.6672 MHz, C15M), the IIci (25.00 MHz) and the IIfx (40.00
 *   MHz); the Macintosh IIvx Developer Note ("Features") for the IIvx and
 *   Performa 600 (31.3344 MHz) and the IIvi (15.6672 MHz); Apple's
 *   published specifications (whole MHz) for the others.
 *   emulation.cpu_mhz defaults to it, and instructions take their cycles
 *   from Motorola's tables (src/core/cpu/m68k_timing.c).
 * - gestalt is Apple's Gestalt machine type (gestaltMachineType); the ROM
 *   stores it as a box flag, the type minus 6 (docs/NOTES.md). Several
 *   machines of one ROM share a type (all 630s report 98, the PowerBook 520
 *   family 72).
 * - ram_mb is the shipped configuration, raised to 4 MiB (the 68000 Macs
 *   whose boards take less offer it through their legal sizes);
 *   ram_max_mb is the documented maximum with the largest SIMMs of the time.
 *   The 128K, 512K and 512Ke have their soldered RAM (ram_kb), whatever
 *   emulation.ram_mb says.
 * - The 68000 Macs (Guide to the Macintosh Family Hardware, 2nd ed.,
 *   chapters 3 and 4 and appendix A): the 68000 at C15M/2 = 7.8336 MHz in
 *   the 128K, 512K, 512Ke, Plus, SE and Classic, the 68HC000 at C15M =
 *   15.6672 MHz in the Portable and PowerBook 100. The 128K and 512K predate
 *   Gestalt (no System that has it runs on their 64 KiB ROM): type 0.
 * - year is the year Apple introduced the machine, form its kind
 *   (MC_FORM_*): everymac.com's and Apple's specifications.
 * - The display is the built-in video or panel; for machines without one
 *   (Mac II family, IIfx, IIvx, Quadra 900/950, 840AV) a typical monitor.
 *   All-in-one and compact Macs get the crt look, PowerBooks the lcd look.
 * - The Old World PowerPC Macintoshes (docs/NOTES.md):
 *   family MC_CPU_PPC, an FPU always. gestalt from Apple's lists: Technote
 *   OV16 ("Gestalt & _SysEnvirons", 1994: the 6100/60 75, 7100/66 112,
 *   8100/80 65, 8100/110 40, 9150 39) and Technical Q&A HW 29 ("Gestalt
 *   Values for Recent Macintosh Models", 1996 and 1998: 5200/5300 41,
 *   6200/6300 42, 6400 58, 7600 68, 5400 74, Duo 2300c 124, 1400 310,
 *   PowerBook G3 Series 312 and 314, G3 510, Twentieth Anniversary 512);
 *   the others (7200 108, 7300 109, 7500 68, 8500 69, 9500 67, 8600 69,
 *   9600 67, 5500 512, 6500 513) as Apple's Gestalt.h of the time gives
 *   them; the later PDM models' (7100/80 47, 8100/100 55, 9150/120 57)
 *   from Apple's specifications. The ROMs agree where they hold ProductInfo records
 *   (box flag + 6, docs/NOTES.md): 9FEB69B3 sets the box flag from the clock
 *   the nanokernel measures (its table at $FFC0139C: $45 at 60 MHz on the
 *   6100, $6A at 66 MHz on the 7100, $3B at 80 MHz on the 8100, so 75, 112
 *   and 65; the 8100's ProductInfo record alone holds 58, 64); 6F5724C0
 *   holds 52 (6400) and 68 (5400). ram_mb is the shipped configuration,
 *   ram_max_mb the most the board takes with the largest DIMMs that work
 *   (everymac.com's "actual" maximum where it exceeds Apple's); cpu_hz
 *   the speed variant whose board is emulated (the 7300's 168 MHz is its
 *   Developer Note's actual clock); the display the
 *   built-in video's or panel's usual mode (a typical monitor for the 9500
 *   and 9600, which have none). */
#include "models.h"
#include "config.h"
#include "romdb.h"
#include <stdio.h>
#include <string.h>

#define PLAIN MC_LOOK_PLAIN, MC_MONITOR_COLOR, MC_MON_COLOR | MC_MON_MODULAR_MONO
#define CRT_COLOR MC_LOOK_CRT, MC_MONITOR_COLOR, MC_MON_COLOR
#define CRT_WHITE MC_LOOK_CRT, MC_MONITOR_MONO, MC_MON_MONO
#define LCD_COLOR MC_LOOK_LCD, MC_MONITOR_COLOR, MC_MON_COLOR
#define LCD_GRAY MC_LOOK_LCD, MC_MONITOR_LCD_GRAY, MC_MON_LCD_GRAY
#define C MC_FORM_COMPACT
#define D MC_FORM_DESKTOP
#define A MC_FORM_ALL_IN_ONE
#define L MC_FORM_LAPTOP
#define S MC_FORM_SERVER
#define G MC_FORM_CONSOLE
#define M(key, name, rom, gestalt, fpu, cpu, hz, ram, max, w, h, d, look, yr, kind)     {key, name, rom, gestalt, fpu, cpu, hz, ram, max, w, h, d, look, .year = yr, .form = kind}
#define MK(key, name, rom, gestalt, cpu, hz, kib, look, yr, kind)     {key, name, rom, gestalt, 0, cpu, hz, 1, 1, 512, 342, 1, look, kib, .year = yr, .form = kind}
#define P(key, name, rom, gestalt, cpu, hz, ram, max, w, h, d, look, yr, kind)     {key, name, rom, gestalt, 1, cpu, hz, ram, max, w, h, d, look, 0, MC_CPU_PPC, 0, .year = yr, .form = kind}
#define PC(key, name, rom, gestalt, cpu, hz, ram, max, w, h, d, look, yr)     {key, name, rom, gestalt, 1, cpu, hz, ram, max, w, h, d, look, 0, MC_CPU_PPC, 1, .year = yr, .form = MC_FORM_CONSOLE}

static const mc_model models[] = {
    /* The Built-in machine (models.h): the 128K's board with Livadix's ROM. */
    MK(MC_MODEL_BUILTIN, "Built-in", MC_BUILTIN_ROM, 0, "68000 7.8336 MHz", 7833600, 128, CRT_WHITE, 0, C),
    /* 28BA61CE, 28BA4E50: the 64 KiB ROMs; 4D1EEEE1, 4D1EEAE1, 4D1F8172:
     * the Plus's three 128 KiB ROMs (the 512Ke runs the last). The Plus
     * takes 1, 2, 2.5 or 4 MB in its four SIMM sockets. */
    MK("mac-128k", "Macintosh 128K", 0x28ba61ce, 0, "68000 7.8336 MHz", 7833600, 128, CRT_WHITE, 1984, C),
    MK("mac-512k", "Macintosh 512K", 0x28ba4e50, 0, "68000 7.8336 MHz", 7833600, 512, CRT_WHITE, 1984, C),
    MK("mac-512ke", "Macintosh 512Ke", 0x4d1f8172, 3, "68000 7.8336 MHz", 7833600, 512, CRT_WHITE, 1986, C),
    M("mac-plus", "Macintosh Plus", 0x4d1f8172, 4, 0, "68000 7.8336 MHz", 7833600, 4, 4, 512, 342, 1, CRT_WHITE, 1986, C),
    M("mac-plus-v1", "Macintosh Plus (first ROM)", 0x4d1eeee1, 4, 0, "68000 7.8336 MHz", 7833600, 4, 4, 512, 342, 1, CRT_WHITE, 1986, C),
    M("mac-plus-v2", "Macintosh Plus (second ROM)", 0x4d1eeae1, 4, 0, "68000 7.8336 MHz", 7833600, 4, 4, 512, 342, 1, CRT_WHITE, 1986, C),
    /* B2E362A8, B306E171, A49F9914 (with the System 6.0.3 ROM disk). */
    M("mac-se", "Macintosh SE", 0xb2e362a8, 5, 0, "68000 7.8336 MHz", 7833600, 4, 4, 512, 342, 1, CRT_WHITE, 1987, C),
    M("mac-se-fdhd", "Macintosh SE FDHD", 0xb306e171, 5, 0, "68000 7.8336 MHz", 7833600, 4, 4, 512, 342, 1, CRT_WHITE, 1989, C),
    M("mac-classic", "Macintosh Classic", 0xa49f9914, 17, 0, "68000 7.8336 MHz", 7833600, 4, 4, 512, 342, 1, CRT_WHITE, 1990, C),
    /* 96CA3846, 96645F9C: static RAM, the 640x400 one-bit LCD. The
     * Portable takes 1 MB on the board and up to 8 MB on its card. */
    M("mac-portable", "Macintosh Portable", 0x96ca3846, 10, 0, "68HC000 15.6672 MHz", 15667200, 4, 9, 640, 400, 1, LCD_GRAY, 1989, L),
    M("powerbook-100", "PowerBook 100", 0x96645f9c, 24, 0, "68HC000 15.6672 MHz", 15667200, 4, 8, 640, 400, 1, LCD_GRAY, 1991, L),
    /* 97851DB6, 9779D2C4: Macintosh II, 256 KiB 24-bit ROMs. Max 8 MiB with
     * these ROMs (68 MiB needs the FDHD ROM). The 8-bit 640x480 is the
     * Macintosh II Video Card with its VRAM expansion. */
    M("mac-ii-v1", "Macintosh II (1987 ROM)", 0x97851db6, 6, 1, "68020 15.6672 MHz", 15667200, 4, 8, 640, 480, 8, PLAIN, 1987, D),
    M("mac-ii-v2", "Macintosh II", 0x9779d2c4, 6, 1, "68020 15.6672 MHz", 15667200, 4, 8, 640, 480, 8, PLAIN, 1987, D),
    /* 97221136: the 1988 256 KiB ROM. */
    M("mac-iix", "Macintosh IIx", 0x97221136, 7, 1, "68030 15.6672 MHz", 15667200, 4, 128, 640, 480, 8, PLAIN, 1988, D),
    M("mac-ii-fdhd", "Macintosh II (FDHD)", 0x97221136, 6, 1, "68020 15.6672 MHz", 15667200, 4, 68, 640, 480, 8, PLAIN, 1989, D),
    M("mac-iicx", "Macintosh IIcx", 0x97221136, 8, 1, "68030 15.6672 MHz", 15667200, 4, 128, 640, 480, 8, PLAIN, 1989, D),
    M("se-30", "Macintosh SE/30", 0x97221136, 9, 1, "68030 15.6672 MHz", 15667200, 4, 128, 512, 342, 1, CRT_WHITE, 1989, C),
    M("iici", "Macintosh IIci", 0x368cadfe, 11, 1, "68030 25 MHz", 25000000, 4, 128, 640, 480, 8, PLAIN, 1989, D),
    M("iifx", "Macintosh IIfx", 0x4147dd77, 13, 1, "68030 40 MHz", 40000000, 4, 128, 640, 480, 8, PLAIN, 1990, D),
    M("lc", "Macintosh LC", 0x350eacf0, 19, 0, "68020 16 MHz", 16000000, 4, 10, 512, 384, 8, PLAIN, 1990, D),
    M("iisi", "Macintosh IIsi", 0x36b7fb6c, 18, 0, "68030 20 MHz", 20000000, 5, 65, 640, 480, 8, PLAIN, 1990, D),
    M("classic-ii", "Macintosh Classic II", 0x3193670e, 23, 0, "68030 16 MHz", 16000000, 4, 10, 512, 342, 1, CRT_WHITE, 1991, C),
    /* 420DBFF3. The Quadra 700's built-in video gives 8 bits at 1152x870
     * only with its VRAM expansion. */
    M("quadra-700", "Macintosh Quadra 700", 0x420dbff3, 22, 1, "68040 25 MHz", 25000000, 4, 68, 1152, 870, 8, PLAIN, 1991, D),
    M("quadra-900", "Macintosh Quadra 900", 0x420dbff3, 20, 1, "68040 25 MHz", 25000000, 4, 256, 1152, 870, 8, PLAIN, 1991, D),
    M("powerbook-140", "PowerBook 140", 0x420dbff3, 25, 0, "68030 16 MHz", 16000000, 4, 8, 640, 400, 1, LCD_GRAY, 1991, L),
    M("powerbook-170", "PowerBook 170", 0x420dbff3, 21, 1, "68030 25 MHz", 25000000, 4, 8, 640, 400, 1, LCD_GRAY, 1991, L),
    /* The PowerBook 145 and 145B: the PowerBook 140's board at 25 MHz
     * (the 145B with 4 MB on the board), the same ROM. */
    M("powerbook-145", "PowerBook 145", 0x420dbff3, 54, 0, "68030 25 MHz", 25000000, 4, 8, 640, 400, 1, LCD_GRAY, 1992, L),
    M("powerbook-145b", "PowerBook 145B", 0x420dbff3, 54, 0, "68030 25 MHz", 25000000, 4, 8, 640, 400, 1, LCD_GRAY, 1993, L),
    M("lc-ii", "Macintosh LC II", 0x35c28f5f, 37, 0, "68030 16 MHz", 16000000, 4, 10, 512, 384, 8, PLAIN, 1992, D),
    M("quadra-950", "Macintosh Quadra 950", 0x3dc27823, 26, 1, "68040 33 MHz", 33000000, 8, 256, 1152, 870, 8, PLAIN, 1992, D),
    /* 4957EB49. The IIvi and Performa 600 had an FPU socket, empty as shipped. */
    M("iivx", "Macintosh IIvx", 0x4957eb49, 48, 1, "68030 32 MHz", 31334400, 4, 68, 640, 480, 8, PLAIN, 1992, D),
    M("iivi", "Macintosh IIvi", 0x4957eb49, 44, 0, "68030 16 MHz", 15667200, 4, 68, 640, 480, 8, PLAIN, 1992, D),
    M("performa-600", "Macintosh Performa 600", 0x4957eb49, 45, 0, "68030 32 MHz", 31334400, 4, 68, 640, 480, 8, PLAIN, 1992, D),
    /* E33B2724. The 160, 165 and 180 panels show 16 grays (4 bits). */
    M("powerbook-160", "PowerBook 160", 0xe33b2724, 34, 0, "68030 25 MHz", 25000000, 4, 14, 640, 400, 4, LCD_GRAY, 1992, L),
    M("powerbook-165", "PowerBook 165", 0xe33b2724, 84, 0, "68030 33 MHz", 33000000, 4, 14, 640, 400, 4, LCD_GRAY, 1993, L),
    M("powerbook-165c", "PowerBook 165c", 0xe33b2724, 50, 1, "68030 33 MHz", 33000000, 4, 14, 640, 400, 8, LCD_COLOR, 1993, L),
    M("powerbook-180", "PowerBook 180", 0xe33b2724, 33, 1, "68030 33 MHz", 33000000, 4, 14, 640, 400, 4, LCD_GRAY, 1992, L),
    M("powerbook-180c", "PowerBook 180c", 0xe33b2724, 71, 1, "68030 33 MHz", 33000000, 4, 14, 640, 480, 8, LCD_COLOR, 1993, L),
    /* ECFA989B: 16-gray panels. */
    M("powerbook-duo-210", "PowerBook Duo 210", 0xecfa989b, 29, 0, "68030 25 MHz", 25000000, 4, 24, 640, 400, 4, LCD_GRAY, 1992, L),
    M("powerbook-duo-230", "PowerBook Duo 230", 0xecfa989b, 32, 0, "68030 33 MHz", 33000000, 4, 24, 640, 400, 4, LCD_GRAY, 1992, L),
    M("powerbook-duo-250", "PowerBook Duo 250", 0xecfa989b, 38, 0, "68030 33 MHz", 33000000, 4, 24, 640, 400, 4, LCD_GRAY, 1993, L),
    /* EC904829 is the LC III's first ROM, ECBBC41C its later one. */
    M("lc-iii-v1", "Macintosh LC III (early ROM)", 0xec904829, 27, 0, "68030 25 MHz", 25000000, 4, 36, 640, 480, 8, PLAIN, 1993, D),
    M("lc-iii", "Macintosh LC III", 0xecbbc41c, 27, 0, "68030 25 MHz", 25000000, 4, 36, 640, 480, 8, PLAIN, 1993, D),
    /* Apple LC III+ Technical Specifications: 33 MHz, Gestalt 62. */
    M("lc-iii-plus", "Macintosh LC III+", 0xecbbc41c, 62, 0, "68030 33 MHz", 33000000, 4, 36, 640, 480, 8, PLAIN, 1993, D),
    M("color-classic", "Macintosh Color Classic", 0xecd99dc0, 49, 0, "68030 16 MHz", 16000000, 4, 10, 512, 384, 8, CRT_COLOR, 1993, C),
    /* F1A6F343 and F1ACAD13 both list the 610 and 650. Uncertain: which
     * shipped in which; the Centris (Feb 1993) get the first, the Quadras
     * (the 800 in Feb 1993, the 610/650 in Oct 1993) the second. The 610 and
     * 650 commonly drove a 640x480 monitor; the 800 a 1152x870 one. */
    M("centris-610", "Macintosh Centris 610", 0xf1a6f343, 52, 0, "68LC040 20 MHz", 20000000, 4, 68, 640, 480, 8, PLAIN, 1993, D),
    M("centris-650", "Macintosh Centris 650", 0xf1a6f343, 30, 1, "68040 25 MHz", 25000000, 8, 136, 640, 480, 8, PLAIN, 1993, D),
    M("quadra-800", "Macintosh Quadra 800", 0xf1acad13, 35, 1, "68040 33 MHz", 33000000, 8, 136, 1152, 870, 8, PLAIN, 1993, D),
    M("quadra-610", "Macintosh Quadra 610", 0xf1acad13, 53, 1, "68040 25 MHz", 25000000, 8, 68, 640, 480, 8, PLAIN, 1993, D),
    M("quadra-650", "Macintosh Quadra 650", 0xf1acad13, 36, 1, "68040 33 MHz", 33000000, 8, 136, 640, 480, 8, PLAIN, 1993, D),
    M("powerbook-duo-270c", "PowerBook Duo 270c", 0x0024d346, 77, 1, "68030 33 MHz", 33000000, 4, 32, 640, 480, 8, LCD_COLOR, 1993, L),
    /* EDE66CBD. Uncertain: the Macintosh TV's type 88 is not in this ROM's
     * ProductInfo lists (docs/NOTES.md); the ROM may derive it at run time. */
    M("color-classic-ii", "Macintosh Color Classic II", 0xede66cbd, 83, 0, "68030 33 MHz", 33000000, 4, 36, 512, 384, 8, CRT_COLOR, 1993, C),
    /* LC 520 Developer Note, chapters 1, 2 and 4: 25 MHz, Gestalt 56,
     * 4 MB soldered RAM, expandable to 36 MB. */
    M("lc-520", "Macintosh LC 520", 0xede66cbd, 56, 0, "68030 25 MHz", 25000000, 4, 36, 640, 480, 8, CRT_COLOR, 1993, A),
    M("lc-550", "Macintosh LC 550", 0xede66cbd, 80, 0, "68030 33 MHz", 33000000, 5, 36, 640, 480, 8, CRT_COLOR, 1994, A),
    M("performa-275", "Macintosh Performa 275", 0xede66cbd, 83, 0, "68030 33 MHz", 33000000, 4, 36, 512, 384, 8, CRT_COLOR, 1993, C),
    M("performa-550", "Macintosh Performa 550", 0xede66cbd, 80, 0, "68030 33 MHz", 33000000, 5, 36, 640, 480, 8, CRT_COLOR, 1993, A),
    M("performa-560", "Macintosh Performa 560", 0xede66cbd, 80, 0, "68030 33 MHz", 33000000, 5, 36, 640, 480, 8, CRT_COLOR, 1994, A),
    M("macintosh-tv", "Macintosh TV", 0xeaf1678d, 88, 0, "68030 32 MHz", 32000000, 5, 8, 640, 480, 8, CRT_COLOR, 1993, A),
    /* FF7439EE. The 475/605 video gives 8 bits at 832x624; the 575 family
     * is an all-in-one with a 14" 640x480 tube, in thousands of colors with
     * the 1 MB VRAM option (256 with the 512 KB shipped). */
    M("quadra-605", "Macintosh Quadra 605", 0xff7439ee, 94, 0, "68LC040 25 MHz", 25000000, 4, 36, 832, 624, 8, PLAIN, 1993, D),
    M("lc-475", "Macintosh LC 475", 0xff7439ee, 89, 0, "68LC040 25 MHz", 25000000, 4, 36, 832, 624, 8, PLAIN, 1993, D),
    M("performa-475", "Macintosh Performa 475", 0xff7439ee, 89, 0, "68LC040 25 MHz", 25000000, 4, 36, 832, 624, 8, PLAIN, 1993, D),
    M("performa-476", "Macintosh Performa 476", 0xff7439ee, 89, 0, "68LC040 25 MHz", 25000000, 4, 36, 832, 624, 8, PLAIN, 1993, D),
    M("lc-575", "Macintosh LC 575", 0xff7439ee, 92, 0, "68LC040 33 MHz", 33000000, 5, 36, 640, 480, 16, CRT_COLOR, 1994, A),
    M("performa-575", "Macintosh Performa 575", 0xff7439ee, 92, 0, "68LC040 33 MHz", 33000000, 5, 36, 640, 480, 16, CRT_COLOR, 1994, A),
    M("performa-577", "Macintosh Performa 577", 0xff7439ee, 92, 0, "68LC040 33 MHz", 33000000, 5, 36, 640, 480, 16, CRT_COLOR, 1994, A),
    M("performa-578", "Macintosh Performa 578", 0xff7439ee, 92, 0, "68LC040 33 MHz", 33000000, 8, 36, 640, 480, 16, CRT_COLOR, 1994, A),
    M("powerbook-duo-280", "PowerBook Duo 280", 0x015621d7, 102, 0, "68LC040 33 MHz", 33000000, 4, 40, 640, 480, 4, LCD_GRAY, 1994, L),
    M("powerbook-duo-280c", "PowerBook Duo 280c", 0x015621d7, 103, 0, "68LC040 33 MHz", 33000000, 4, 40, 640, 480, 8, LCD_COLOR, 1994, L),
    /* B6909089: one machine type for the family. Uncertain: the 540's
     * 64-gray panel is given 8-bit grays. The 540c's panel shows 256 colors
     * at 640x480 (thousands only at 640x400), its CSC 8 bits. */
    M("powerbook-520", "PowerBook 520", 0xb6909089, 72, 0, "68LC040 25 MHz", 25000000, 4, 36, 640, 480, 4, LCD_GRAY, 1994, L),
    M("powerbook-520c", "PowerBook 520c", 0xb6909089, 72, 0, "68LC040 25 MHz", 25000000, 4, 36, 640, 480, 8, LCD_COLOR, 1994, L),
    M("powerbook-540", "PowerBook 540", 0xb6909089, 72, 0, "68LC040 33 MHz", 33000000, 4, 36, 640, 480, 8, LCD_GRAY, 1994, L),
    M("powerbook-540c", "PowerBook 540c", 0xb6909089, 72, 0, "68LC040 33 MHz", 33000000, 4, 36, 640, 480, 8, LCD_COLOR, 1994, L),
    /* 06684214: all three report type 98 (box flag 92 in the ROM's second
     * ProductInfo list). The 630 video gives 8 bits at 832x624. */
    M("quadra-630", "Macintosh Quadra 630", 0x06684214, 98, 1, "68040 33 MHz", 33000000, 8, 36, 832, 624, 8, PLAIN, 1994, D),
    M("lc-630", "Macintosh LC 630", 0x06684214, 98, 0, "68LC040 33 MHz", 33000000, 8, 36, 832, 624, 8, PLAIN, 1994, D),
    M("performa-630", "Macintosh Performa 630", 0x06684214, 98, 0, "68LC040 33 MHz", 33000000, 8, 36, 832, 624, 8, PLAIN, 1994, D),
    M("powerbook-150", "PowerBook 150", 0xfda22562, 115, 0, "68030 33 MHz", 33000000, 4, 40, 640, 480, 2, LCD_GRAY, 1994, L),
    /* 5BF10FD1. The 660AV's DRAM video gives thousands at 640x480. */
    M("quadra-840av", "Macintosh Quadra 840AV", 0x5bf10fd1, 78, 1, "68040 40 MHz", 40000000, 8, 128, 1152, 870, 8, PLAIN, 1993, D),
    M("quadra-660av", "Macintosh Quadra 660AV", 0x5bf10fd1, 60, 1, "68040 25 MHz", 25000000, 8, 68, 640, 480, 16, PLAIN, 1993, D),
    /* 064DC91D: an all-in-one with a 14" tube in thousands of colors. */
    M("performa-580", "Macintosh Performa 580", 0x064dc91d, 99, 0, "68LC040 33 MHz", 33000000, 8, 52, 640, 480, 16, CRT_COLOR, 1995, A),
    M("lc-580", "Macintosh LC 580", 0x064dc91d, 99, 0, "68LC040 33 MHz", 33000000, 8, 52, 640, 480, 16, CRT_COLOR, 1995, A),
    M("performa-588", "Macintosh Performa 588", 0x064dc91d, 99, 0, "68LC040 33 MHz", 33000000, 8, 52, 640, 480, 16, CRT_COLOR, 1995, A),
    M("powerbook-190", "PowerBook 190", 0x4d27039c, 85, 0, "68LC040 33 MHz", 33000000, 8, 40, 640, 480, 4, LCD_GRAY, 1995, L),
    M("powerbook-190cs", "PowerBook 190cs", 0x4d27039c, 85, 0, "68LC040 33 MHz", 33000000, 8, 40, 640, 480, 8, LCD_COLOR, 1995, L),
    /* The PowerPC Macintoshes. 9FEB69B3: the first PDM ROM; the 6100, 7100
     * and 8100 are emulated (src/hw/ppc/machines/pdm.c). The 6150 is the
     * 6100/66 (box flag $5E at 66 MHz: 100), the 8150 the 8100/80. */
    P("power-macintosh-6100", "Power Macintosh 6100", 0x9feb69b3, 75, "PowerPC 601 60 MHz", 60000000, 8, 72, 640, 480, 8, PLAIN, 1994, D),
    P("power-macintosh-7100", "Power Macintosh 7100", 0x9feb69b3, 112, "PowerPC 601 66 MHz", 66000000, 8, 136, 640, 480, 8, PLAIN, 1994, D),
    P("power-macintosh-8100", "Power Macintosh 8100", 0x9feb69b3, 65, "PowerPC 601 80 MHz", 80000000, 8, 264, 640, 480, 8, PLAIN, 1994, D),
    P("workgroup-server-6150", "Workgroup Server 6150", 0x9feb69b3, 100, "PowerPC 601 66 MHz", 66000000, 16, 72, 640, 480, 8, PLAIN, 1994, S),
    P("workgroup-server-8150", "Workgroup Server 8150", 0x9feb69b3, 65, "PowerPC 601 80 MHz", 80000000, 16, 264, 640, 480, 8, PLAIN, 1994, S),
    /* 9C7C98F7 and 9B037F6F: the 9150's own ROMs (box flag 33). */
    P("workgroup-server-9150-80", "Workgroup Server 9150/80", 0x9c7c98f7, 39, "PowerPC 601 80 MHz", 80000000, 16, 264, 640, 480, 8, PLAIN, 1994, S),
    /* 9B7A3AAD: the later PDM ROM of the speed-bumped models. */
    P("power-macintosh-7100-80", "Power Macintosh 7100/80", 0x9b7a3aad, 47, "PowerPC 601 80 MHz", 80000000, 8, 136, 640, 480, 8, PLAIN, 1995, D),
    P("power-macintosh-8100-100", "Power Macintosh 8100/100", 0x9b7a3aad, 55, "PowerPC 601 100 MHz", 100000000, 16, 264, 640, 480, 8, PLAIN, 1995, D),
    P("power-macintosh-8100-110", "Power Macintosh 8100/110", 0x9b7a3aad, 40, "PowerPC 601 110 MHz", 110000000, 16, 264, 640, 480, 8, PLAIN, 1994, D),
    P("workgroup-server-9150-120", "Workgroup Server 9150/120", 0x9b037f6f, 57, "PowerPC 601 120 MHz", 120000000, 16, 264, 640, 480, 8, PLAIN, 1995, S),
    /* 63ABFD3F: the emulated Capella boards (machines/capella.c). The
     * 5200/5300 have a fixed 640x480 panel; the 6200/6300 have external
     * Valkyrie monitors. The note ch. 2 and Table 3-10 specify 4/8/16
     * bits through 640x480, 4/8 at 800x600 and 832x624. */
    P("power-macintosh-5200", "Power Macintosh 5200", 0x63abfd3f, 41, "PowerPC 603 75 MHz", 75000000, 8, 64, 640, 480, 16, CRT_COLOR, 1995, A),
    P("power-macintosh-5300", "Power Macintosh 5300", 0x63abfd3f, 41, "PowerPC 603e 100 MHz", 100000000, 16, 64, 640, 480, 16, CRT_COLOR, 1995, A),
    P("power-macintosh-6200", "Power Macintosh 6200", 0x63abfd3f, 42, "PowerPC 603 75 MHz", 75000000, 8, 64, 640, 480, 16, PLAIN, 1995, D),
    P("power-macintosh-6300", "Power Macintosh 6300", 0x63abfd3f, 42, "PowerPC 603e 100 MHz", 100000000, 16, 64, 640, 480, 16, PLAIN, 1995, D),
    /* 96CD923D and 9630C68B: the first PCI generation; the 9500 has no
     * built-in video (a card). */
    P("power-macintosh-7200-v1", "Power Macintosh 7200 (first ROM)", 0x96cd923d, 108, "PowerPC 601 75 MHz", 75000000, 8, 512, 832, 624, 8, PLAIN, 1995, D),
    P("power-macintosh-7200", "Power Macintosh 7200", 0x9630c68b, 108, "PowerPC 601 90 MHz", 90000000, 8, 512, 832, 624, 8, PLAIN, 1995, D),
    P("power-macintosh-7500", "Power Macintosh 7500", 0x9630c68b, 68, "PowerPC 601 100 MHz", 100000000, 16, 1024, 832, 624, 16, PLAIN, 1995, D),
    P("power-macintosh-8500", "Power Macintosh 8500", 0x9630c68b, 69, "PowerPC 604 120 MHz", 120000000, 16, 1024, 832, 624, 16, PLAIN, 1995, D),
    P("power-macintosh-9500", "Power Macintosh 9500", 0x9630c68b, 67, "PowerPC 604 120 MHz", 120000000, 16, 1536, 1152, 870, 8, PLAIN, 1995, D),
    P("powerbook-duo-2300c", "PowerBook Duo 2300c", 0x83c54f75, 124, "PowerPC 603e 100 MHz", 100000000, 20, 56, 640, 480, 8, LCD_COLOR, 1995, L),
    /* 6F5724C0: the 5400 is an all-in-one with a 15" tube. */
    P("power-macintosh-6400", "Power Macintosh 6400", 0x6f5724c0, 58, "PowerPC 603e 180 MHz", 180000000, 16, 136, 640, 480, 16, PLAIN, 1996, D),
    P("power-macintosh-5400", "Power Macintosh 5400", 0x6f5724c0, 74, "PowerPC 603e 120 MHz", 120000000, 16, 136, 640, 480, 16, CRT_COLOR, 1996, A),
    P("powerbook-1400", "PowerBook 1400c", 0x83a21950, 310, "PowerPC 603e 133 MHz", 133000000, 16, 64, 800, 600, 16, LCD_COLOR, 1996, L),
    P("powerbook-1400cs", "PowerBook 1400cs", 0x83a21950, 310, "PowerPC 603e 117 MHz", 117000000, 12, 64, 800, 600, 16, LCD_COLOR, 1996, L),
    /* 960E4BE9 and 960FC647 (the 604e at 250 to 350 MHz): the second PCI
     * generation. */
    P("power-macintosh-7300", "Power Macintosh 7300", 0x960e4be9, 109, "PowerPC 604e 168 MHz", 168000000, 32, 1024, 832, 624, 8, PLAIN, 1997, D),
    P("power-macintosh-7600", "Power Macintosh 7600", 0x960e4be9, 68, "PowerPC 604 132 MHz", 132000000, 16, 1024, 832, 624, 16, PLAIN, 1996, D),
    P("power-macintosh-8600", "Power Macintosh 8600", 0x960e4be9, 69, "PowerPC 604e 200 MHz", 200000000, 32, 1024, 832, 624, 16, PLAIN, 1997, D),
    P("power-macintosh-9600", "Power Macintosh 9600", 0x960e4be9, 67, "PowerPC 604e 200 MHz", 200000000, 32, 1536, 1152, 870, 8, PLAIN, 1997, D),
    /* 6E92FE08: the 5500 an all-in-one (15" tube), the Twentieth
     * Anniversary Macintosh a 12.1" 800x600 panel. */
    P("power-macintosh-6500", "Power Macintosh 6500", 0x6e92fe08, 513, "PowerPC 603e 250 MHz", 250000000, 32, 128, 800, 600, 16, PLAIN, 1997, D),
    P("power-macintosh-5500", "Power Macintosh 5500", 0x6e92fe08, 512, "PowerPC 603e 225 MHz", 225000000, 32, 128, 640, 480, 16, CRT_COLOR, 1997, A),
    P("twentieth-anniversary-macintosh", "Twentieth Anniversary Macintosh", 0x6e92fe08, 512, "PowerPC 603e 250 MHz", 250000000, 32, 128,
      800, 600, 16, LCD_COLOR, 1997, A),
    P("power-macintosh-8600-300", "Power Macintosh 8600/300", 0x960fc647, 69, "PowerPC 604e 300 MHz", 300000000, 32, 1024, 832, 624, 16, PLAIN, 1997, D),
    P("power-macintosh-9600-350", "Power Macintosh 9600/350", 0x960fc647, 67, "PowerPC 604e 350 MHz", 350000000, 64, 1536, 1152, 870, 8, PLAIN, 1997, D),
    /* 79D68D63 and 78F57389: the G3's ATI Rage II+ (the Rage Pro later);
     * the All-in-One a 15" tube. */
    /* Beige G3 boards: machines/g3.c, PCI Livadix Display Card fallback. */
    P("power-macintosh-g3-v1", "Power Macintosh G3 (first ROM)", 0x79d68d63, 510, "PowerPC 750 233 MHz", 233000000, 32, 768, 1024, 768, 16, PLAIN, 1997, D),
    P("power-macintosh-g3", "Power Macintosh G3", 0x78f57389, 510, "PowerPC 750 266 MHz", 266000000, 32, 768, 1024, 768, 16, PLAIN, 1998, D),
    P("power-macintosh-g3-minitower", "Power Macintosh G3 Minitower", 0x78f57389, 510, "PowerPC 750 266 MHz", 266000000, 32, 768, 1024, 768, 16,
      PLAIN, 1997, D),
    P("power-macintosh-g3-all-in-one", "Power Macintosh G3 All-in-One", 0x78f57389, 510, "PowerPC 750 233 MHz", 233000000, 32, 384, 800, 600, 16,
      CRT_COLOR, 1998, A),
    P("powerbook-g3-wallstreet", "PowerBook G3 Series", 0xcbb01212, 312, "PowerPC 750 233 MHz", 233000000, 32, 512, 1024, 768, 16, LCD_COLOR, 1998, L),
    P("powerbook-g3-pdq", "PowerBook G3 Series (PDQ)", 0xb46ffb63, 314, "PowerPC 750 266 MHz", 266000000, 32, 512, 1024, 768, 16, LCD_COLOR, 1998, L),
    /* The Pippin starts from CD and takes plug-in RAM (Apple TIL 18537).
     * The 6 MiB default preserves existing profiles. Its Gestalt type
     * remains uncertain (no known ProductInfo list in its ROM). */
    PC("pippin", "Bandai Pippin", 0x2bef21b7, 0, "PowerPC 603 66 MHz", 66000000, 6, 21, 640, 480, 16, PLAIN, 1996),
    PC("pippin-dev", "Bandai Pippin (development ROM)", 0x2bf65931, 0, "PowerPC 603 66 MHz", 66000000, 6, 21, 640, 480, 16, PLAIN, 1995),
};
#undef C
#undef D
#undef A
#undef L
#undef S
#undef G
#define COUNT (sizeof models / sizeof *models)

/* The stub machine's own model (models.h, MC_MODEL_STUB): not a Macintosh,
 * so not in the table, the count or model: auto; the index after the
 * table's last. Any ROM file the configuration names with the PowerBook
 * 520's checksum and size serves it (the stub runs no code). */
static const mc_model stub_model =
    M(MC_MODEL_STUB, "Livadix stub machine", 0xb6909089, 0, 0, "none", 16000000, 4, 8, 640, 480, 8, CRT_COLOR, 0, MC_FORM_DESKTOP);

size_t mc_model_count(void) { return COUNT; }
const char *mc_model_form_name(unsigned form) {
    static const char *const names[MC_FORMS] = {"Compact", "Desktop", "All-in-one", "Laptop", "Server", "Console"};
    return form < MC_FORMS ? names[form] : "";
}
void mc_model_cpu_type(const mc_model *m, char *out, size_t capacity) {
    if (!capacity) return;
    out[0] = 0;
    if (!m) return;
    /* The processor is the text before its clock (the last word with digits before " MHz"). */
    const char *mhz = strstr(m->cpu, " MHz");
    size_t n = mhz ? (size_t)(mhz - m->cpu) : strlen(m->cpu);
    while (mhz && n && m->cpu[n - 1] != ' ') n--;
    while (mhz && n && m->cpu[n - 1] == ' ') n--;
    if (!strncmp(m->cpu, "68HC000", 7)) snprintf(out, capacity, "68000");
    else snprintf(out, capacity, "%.*s", (int)n, m->cpu);
}
const mc_model *mc_model_entry(size_t index) { return index < COUNT ? &models[index] : index == COUNT ? &stub_model : NULL; }
const mc_model *mc_model_find(const char *key) {
    for (size_t i = 0; key && i < COUNT; ++i) if (!strcmp(models[i].key, key)) return &models[i];
    return key && !strcmp(key, MC_MODEL_STUB) ? &stub_model : NULL;
}
const char *mc_model_controller_firmware(const mc_model *m) {
    static const struct { const char *firmware, *models; } map[] = {
        {"egret-341s0850", " lc lc-ii "},
        {"egret-341s0851", " classic-ii iivx iivi performa-600 lc-iii-v1 lc-iii lc-iii-plus "},
        {"egret-344s0100", " iisi "},
        {"cuda-341s0060", " lc-520 lc-550 color-classic-ii performa-275 performa-550 performa-560 quadra-630 lc-630 performa-630 lc-580 "
                          "performa-580 performa-588 power-macintosh-5200 power-macintosh-5300 power-macintosh-6200 power-macintosh-6300 "},
        {"cuda-341s0417", " color-classic "},
        {"cuda-341s0788", " lc-475 lc-575 quadra-605 performa-475 performa-476 performa-575 performa-577 performa-578 quadra-660av "
                          "quadra-840av "},
        {"cuda-341s0789", " macintosh-tv "},
        {"pmu-pb140", " powerbook-140 powerbook-145 powerbook-145b powerbook-170 "},
        {"pmu-pb160", " powerbook-160 powerbook-165 powerbook-165c powerbook-180 powerbook-180c "},
        {"pmu-portable", " mac-portable powerbook-100 "},
        {"pmu-pge-boot", " powerbook-duo-210 powerbook-duo-230 powerbook-duo-250 "},
    };
    char key[80];
    if (!m) return NULL;
    snprintf(key, sizeof key, " %s ", m->key);
    for (size_t i = 0; i < sizeof map / sizeof *map; ++i) if (strstr(map[i].models, key)) return map[i].firmware;
    return NULL;
}
size_t mc_model_index(const mc_model *p) { return p == &stub_model ? COUNT : p ? (size_t)(p - models) : 0; }
int mc_model_ppc(const mc_model *p) { return p && p->family == MC_CPU_PPC; }
int mc_model_builtin(const mc_model *p) { return p && !strcmp(p->key, MC_MODEL_BUILTIN); }
const mc_known_rom *mc_model_rom(const mc_model *p) {
    for (size_t i = 0; p && i < mc_romdb_count(); ++i)
        if (mc_romdb_entry(i)->checksum == p->rom) return mc_romdb_entry(i);
    return NULL;
}

int mc_model_pmmu_socket(const mc_model *model) {
    return model && (!strcmp(model->key, "mac-ii-v1") || !strcmp(model->key, "mac-ii-v2") || !strcmp(model->key, "mac-ii-fdhd"));
}
