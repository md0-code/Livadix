/* Models (docs/NOTES.md, "Models"): one per real Macintosh
 * whose ROM is in docs/NOTES.md. A model supplies the defaults of
 * `model:`; `model: auto` (the default) picks one (config.c). */

#ifndef MC_MODELS_H
#define MC_MODELS_H

#include <stddef.h>
#include <stdint.h>

/* Monitors a model allows: a built-in screen is fixed to its own; a modular
 * Mac drives any monitor (color) and, at the sizes and depths of Apple's
 * monochrome displays (mc_config_monitor_check), the paper-white one. */
enum { MC_MON_COLOR = 1, MC_MON_MONO = 2, MC_MON_LCD_GRAY = 4, MC_MON_MODULAR_MONO = 8 };

/* The processor family: a PowerPC model always has its FPU, never runs in
 * 24-bit mode and runs 68k code in the ROM's own 68LC040 emulator. */
enum { MC_CPU_68K = 0, MC_CPU_PPC = 1 };

/* The kind of machine (the configuration window's Type filter): a compact
 * Mac (the built-in 9" or 10" tube), a desktop or tower driving its own
 * monitor, an all-in-one with a built-in color tube or panel, a laptop, a
 * server, a game console. */
enum { MC_FORM_COMPACT, MC_FORM_DESKTOP, MC_FORM_ALL_IN_ONE, MC_FORM_LAPTOP, MC_FORM_SERVER, MC_FORM_CONSOLE, MC_FORMS };

typedef struct mc_model {
    const char *key, *name;
    uint32_t rom;              /* ROM header checksum */
    uint16_t gestalt;          /* Gestalt machine type the machine reports (0: none or not known) */
    uint8_t fpu;               /* the real machine had a floating-point unit */
    const char *cpu;           /* the real processor and clock */
    uint32_t cpu_hz;           /* that processor's clock (emulation.cpu_mhz's default) */
    uint32_t ram_mb;           /* default RAM in MiB */
    uint32_t ram_max_mb;       /* the real machine's maximum */
    uint32_t width, height, depth; /* built-in or typical display */
    uint32_t look, monitor;   /* MC_LOOK_*, MC_MONITOR_* (config.h) */
    uint32_t monitors;        /* the monitors the real hardware offered: MC_PHOS_* */
    uint32_t ram_kb;          /* the soldered RAM in KiB of a board with less than 1 MiB (128K, 512K,
                                 512Ke), which emulation.ram_mb does not change; 0: ram_mb */
    uint8_t family;           /* MC_CPU_* */
    uint8_t console;          /* a game console (the Pippin): it starts its own discs, has no row in the
                                 compatibility table and is never model: auto's choice */
    uint16_t year;            /* the year Apple introduced it */
    uint8_t form;             /* MC_FORM_* */
} mc_model;

size_t mc_model_count(void);
/* The name of a form (MC_FORM_*): "Compact", "Desktop", ... */
const char *mc_model_form_name(unsigned form);
/* The model's processor without its clock ("68030", "PowerPC 603e"); the
 * 68HC000 counts as the 68000. */
void mc_model_cpu_type(const mc_model *model, char *out, size_t capacity);
/* Whether the model's board has the Macintosh II's PMMU socket (the AMU's,
 * which a 68851 replaces: emulation.pmmu). */
int mc_model_pmmu_socket(const mc_model *model);
/* The stub machine's model key: a hidden entry (mc_model_find finds it,
 * mc_model_entry(mc_model_count()) is it) that no real machine serves, for
 * the tests and gates of the machine interface and the shell. */
#define MC_MODEL_STUB "stub"
/* The Built-in machine: Livadix's own boot ROM (src/hw/guest/drivers/bootrom.s,
 * tools/bootrom.py) on the Macintosh 128K's board, the default model; its
 * ROM is embedded in livadix.exe, with checksum 'LVDX'. */
#define MC_MODEL_BUILTIN "builtin"
#define MC_BUILTIN_ROM 0x4C564458u
int mc_model_builtin(const mc_model *model);
const mc_model *mc_model_entry(size_t index);
/* The model named key, or NULL. */
const mc_model *mc_model_find(const char *key);
size_t mc_model_index(const mc_model *model);
/* 1 for a PowerPC model (MC_CPU_PPC). */
int mc_model_ppc(const mc_model *model);
/* The firmware key (romdb.h, mc_firmware_find) of the model's ADB, clock
 * and power controller (Egret, Cuda, the PMU) when it has one with Apple
 * firmware of its own, else NULL (docs/NOTES.md, "Controller firmware"). */
const char *mc_model_controller_firmware(const mc_model *model);
/* The model's ROM in the ROM database (romdb.h). */
struct mc_known_rom;
const struct mc_known_rom *mc_model_rom(const mc_model *model);

#endif /* MC_MODELS_H */
